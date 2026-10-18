"""Thin v2 py_trees leaves over focused headless owner ports."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass, replace
from enum import Enum
import math
from typing import Any, Final

import py_trees

from manyforge_behavior.failure_details import (
    NodeFailureOrigin,
    exception_failure_details,
    output_may_have_acted,
)
from manyforge_behavior.orchestration_ports import (
    PROGRAM_MOTION_STOP_RECOVERY_CLASSES,
    MockSimulationPort,
    MockSimulationRefusedError,
    NodeAttemptIdentity,
    NodeAttemptSource,
    OwnerInvocationPort,
    OwnerInvocationPendingError,
    OwnerInvocationHeldError,
    OwnerInvocationRefusedByPriorityError,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskPort,
    OwnerTaskSettlementPendingError,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramInterventionPort,
    is_ordinary_operator_stop,
    request_identity,
    describe_owner_failure,
)
from manyforge_behavior.interrupt_outcome import (
    InterruptNodeResult,
    contained_stages_from_stop_terminal,
    map_interrupt_result,
)
from manyforge_behavior.nodes.interruption import InterruptedOutcome
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopState
from manyforge_behavior.program_clock import AttemptTime, ProgramClock
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramValueError,
    input_failure_reason,
)

# The node itself failed: its inputs, its own deadline or condition.
_NODE_FAILURE = NodeFailureOrigin("program_node")

# The Kernel retry-eligibility causes an automatic retry acts on.
_AUTOMATIC_RETRY_CAUSES: Final = frozenset({"planning_no_effect", "collision_stop_nominal"})
_DEFAULT_AUTOMATIC_RETRIES: Final = 3
_DEFAULT_AUTOMATIC_RETRY_DELAY_S: Final = 1.0


class _OwnerAdapter(InterruptedOutcome, py_trees.behaviour.Behaviour):
    def __init__(
        self,
        *,
        name: str,
        node_id: str,
        node_instance_id: str,
        attempt_source: NodeAttemptSource,
        parameters: Mapping[str, Any],
        value_sources: Mapping[str, Mapping[str, Any]],
        input_resolver: NodeAttemptInputResolver,
        output_key_parameter: str | None = None,
        blackboard_read_parameters: tuple[str, ...] = (),
        cancellable: bool = False,
        owner_api: str | None = None,
        mock_simulation: MockSimulationPort | None = None,
        audit_sink: Callable[[dict[str, Any]], None] | None = None,
    ) -> None:
        super().__init__(name=name)
        # The catalog entry's `execution.cancellable`: only such a node
        # reports the `interrupted` outcome when it is stopped.
        self.cancellable = cancellable
        # The catalog entry's `execution.owner_api`, named by owner failures.
        self._owner_failure = NodeFailureOrigin("owner", owner_api=owner_api)
        self._node_id = node_id
        self._node_instance_id = node_instance_id
        self._attempt_source = attempt_source
        self._parameters = dict(parameters)
        self._value_sources = {key: dict(source) for key, source in value_sources.items()}
        self._input_resolver = input_resolver
        self._output_key_parameter = output_key_parameter
        self._blackboard_read_parameters = blackboard_read_parameters
        self._inputs: Mapping[str, Any] | None = None
        self._owner_inputs: OwnerNodeInputs | None = None
        self._identity: NodeAttemptIdentity | None = None
        self._input_error: str | None = None
        self._failure_reason_code: str | None = None
        self._failure_message = ""
        self._failure_details: Mapping[str, Any] | None = None
        self._failure_origin: NodeFailureOrigin | None = None
        self._operator_stop_state: OperatorMotionStopState | None = None
        self._held_before_admission: int | None = None
        self._held_after_operator_stop = False
        self._audit_sink = audit_sink
        # Decision 133. The index orders this node's starts within the Run:
        # +1 per attempt on its first send, kept across re-entries.
        self._mock_simulation = mock_simulation
        self._mock_start_index = 0
        self._mock_request: Mapping[str, Any] | None = None
        self._mock_request_attempt: str | None = None
        self._mock_acknowledged_attempt: str | None = None
        self._mock_ended_attempt: str | None = None

    def initialise(self) -> None:
        # py_trees enters (or re-enters) the node: a new series of attempts.
        self._begin_attempt(restart_series=True)

    def _begin_attempt(self, *, restart_series: bool) -> None:
        """The one fresh-attempt start: tree entry, an operator Retry, the
        release of a hold before admission, and an automatic retry.

        Every attempt takes a fresh identity and input snapshot.
        ``restart_series`` is False only for the attempts of one series that
        continue it (a held-before-admission release, an automatic retry).
        """
        # The previous attempt (Retry, hold release, automatic retry) ends.
        self._mock_simulation_end()
        self._clear_interruption()
        self._held_before_admission = None
        self._held_after_operator_stop = False
        self._inputs = None
        self._owner_inputs = None
        self._identity = None
        self._input_error = None
        self._failure_reason_code = None
        self._failure_message = ""
        self._failure_details = None
        self._failure_origin = None
        try:
            self._identity = self._attempt_source.begin(self._node_instance_id)
            snapshot = self._input_resolver.snapshot(
                node_id=self._node_id,
                node_instance_id=self._node_instance_id,
                parameters=self._parameters,
                value_sources=self._value_sources,
                blackboard_read_parameters=self._blackboard_read_parameters,
            )
            self._inputs = snapshot.parameters
            self._owner_inputs = OwnerNodeInputs(
                parameters=self._inputs,
                blackboard=snapshot.blackboard,
            )
        except (KeyError, ProgramValueError, TypeError, ValueError) as exc:
            # The inputs fail before any owner request, whatever outcomes the
            # catalog entry declares (spec 440 §4.6).
            self._input_error = str(exc)
            self._record_node_failure(
                input_failure_reason(exc), str(exc), exception_failure_details(exc)
            )

    @property
    def failure_reason_code(self) -> str | None:
        return self._failure_reason_code

    @property
    def failure_message(self) -> str:
        return self._failure_message

    @property
    def failure_details(self) -> Mapping[str, Any] | None:
        """Bounded details: an input's problem, path and source, or the
        owner's structured cause."""

        return self._failure_details

    @property
    def failure_origin(self) -> NodeFailureOrigin | None:
        """The node itself, or its owner (with the catalog owner API)."""

        return self._failure_origin

    @property
    def attempt_identity(self) -> NodeAttemptIdentity | None:
        return self._identity

    def _write_output(
        self,
        output: Any,
        *,
        owner_metadata: Mapping[str, Any],
        adapter_metadata: Mapping[str, Any],
    ) -> bool:
        if self._output_key_parameter is None:
            return True
        if self._inputs is None:
            return False
        output_key = self._inputs[self._output_key_parameter]
        if output is None:
            self._record_owner_failure("owner_result_invalid", "output is absent")
            return False
        reserved_metadata = {
            "node_id": self._node_id,
            "node_instance_id": self._node_instance_id,
            **adapter_metadata,
        }
        collisions = set(owner_metadata) & set(reserved_metadata)
        if collisions:
            self._record_owner_failure(
                "owner_result_invalid",
                "owner metadata replaces reserved fields " f"{sorted(collisions)}",
            )
            return False
        try:
            self._input_resolver.value_store.write(
                output_key,
                output,
                metadata={
                    **owner_metadata,
                    **reserved_metadata,
                },
            )
        except (KeyError, ProgramValueError, TypeError) as exc:
            self._record_owner_failure("owner_output_rejected", str(exc))
            return False
        return True

    def _record_failure(
        self,
        reason_code: str | None,
        message: str,
        origin: NodeFailureOrigin,
        details: Mapping[str, Any] | None,
    ) -> None:
        reason = reason_code or "owner_result_invalid"
        self._failure_reason_code = reason
        self._failure_message = message
        self._failure_details = details
        self._failure_origin = origin
        self.feedback_message = f"{reason}: {message}".rstrip()

    def _record_owner_failure(
        self,
        reason_code: str | None,
        message: str,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        """The owner refused or failed this node's request."""

        self._record_failure(reason_code, message, self._owner_failure, details)

    def _record_owner_error(self, reason_code: str, error: BaseException) -> None:
        """An owner request raised: its reason and failed reference are
        named once in the message, and travel as bounded details."""

        details = exception_failure_details(error)
        self._record_owner_failure(
            reason_code, describe_owner_failure(str(error), details), details
        )

    def _record_node_failure(
        self,
        reason_code: str,
        message: str,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        """The node itself failed (inputs, deadline, condition)."""

        self._record_failure(reason_code, message, _NODE_FAILURE, details)

    def _mock_simulation_ready(self) -> py_trees.common.Status | None:
        """Decision 133: this attempt's literal Mock simulation is
        acknowledged before the node's action or read.

        None: acknowledged, or nothing authored; act now (an instant read
        reads on this same tick). RUNNING: the reply is unknown; the same
        request (same index and attempt) is resent on the next tick, never
        a new attempt. FAILURE: the host refused it. Only ticks send, so a
        Halt, a Stop or the parent ending the node sends no more.
        """

        rows = self._parameters.get("mock_simulation")
        identity = self._identity
        if not rows or identity is None:
            return None
        attempt_id = identity.node_attempt_id
        if self._mock_acknowledged_attempt == attempt_id:
            return None
        if self._mock_simulation is None:
            self._record_node_failure(
                "mock_simulation_refused", "this Program run has no Mock simulation port"
            )
            return py_trees.common.Status.FAILURE
        if self._mock_request_attempt != attempt_id:
            index = self._mock_start_index + 1
            try:
                request = self._mock_simulation.request(identity, index, rows)
            except Exception as exc:
                self._record_owner_error("mock_simulation_refused", exc)
                return py_trees.common.Status.FAILURE
            self._mock_start_index = index
            self._mock_request = request
            self._mock_request_attempt = attempt_id
        assert self._mock_request is not None  # Built with its attempt.
        try:
            ack = self._mock_simulation.start(self._mock_request)
        except MockSimulationRefusedError as error:
            self._record_owner_failure(
                "mock_simulation_refused",
                describe_owner_failure(f"{error.code}: {error}", error.details),
                error.details,
            )
            return py_trees.common.Status.FAILURE
        except Exception:
            self.feedback_message = "Waiting for Mock simulation acknowledgement"
            return py_trees.common.Status.RUNNING
        self._mock_acknowledged_attempt = attempt_id
        if self._audit_sink is not None:
            rows_by_status = {"applied": [], "inactive": []}
            for row in ack.get("signals", ()):
                rows_by_status[row["status"]].append(dict(row["signal"]))
            self._audit_sink(
                {
                    "kind": "program_node_mock_simulation",
                    "node_instance_id": self._node_instance_id,
                    "node_attempt_id": attempt_id,
                    "node_start_index": self._mock_request["node_start_index"],
                    "started_at_ns": ack.get("started_at_ns"),
                    **rows_by_status,
                }
            )
        return None

    def _mock_simulation_end(self) -> None:
        """Decision 135: the attempt that sent its Mock simulation start
        ended, so its still-pending change is cancelled. Once per attempt;
        a Halt is no end (the tree is not stopped)."""

        attempt_id = self._mock_request_attempt
        if (
            self._mock_request is None
            or self._mock_simulation is None
            or attempt_id is None
            or self._mock_ended_attempt == attempt_id
        ):
            return
        self._mock_ended_attempt = attempt_id
        try:
            # An unknown reply stays pending in the port; the Run's tick
            # resends it unchanged and reports it if it never gets an ACK.
            self._mock_simulation.end(self._mock_request)
        except Exception:
            if self._audit_sink is not None:
                self._audit_sink(
                    {
                        "kind": "program_node_mock_simulation_end_unconfirmed",
                        "node_instance_id": self._node_instance_id,
                        "node_attempt_id": attempt_id,
                        "node_start_index": self._mock_request["node_start_index"],
                    }
                )

    def stop(self, new_status: py_trees.common.Status = py_trees.common.Status.INVALID) -> None:
        # Every end of the node: success, failure, preemption, the parent
        # ending it, and Program Stop/Reset (the tree is invalidated).
        super().stop(new_status)
        self._mock_simulation_end()

    def _wait_for_admission_release(self) -> bool:
        if self._held_before_admission is None:
            return False
        state = self._operator_stop_state
        scope = None if state is None else state.snapshot()
        if (scope is None or state.observation_error() is not None
            or scope.selection_revision <= self._held_before_admission
            or self._node_instance_id in scope.held_nodes):
            return True
        # Proven-unstarted work, fresh attempt after owner release.
        self._begin_attempt(restart_series=False)
        return False

    def _hold_before_admission(self, error: OwnerInvocationHeldError) -> None:
        self._held_before_admission = error.selection_revision
        self.feedback_message = str(error)
        if self._operator_stop_state is not None:
            try:
                self._operator_stop_state.refresh()
            except Exception:
                pass  # Retain the exact fence until an authoritative newer release.

    @property
    def presented_status(self) -> py_trees.common.Status:
        return (py_trees.common.Status.INVALID if self._held_before_admission is not None
                else self.status)

    @property
    def presented_held(self) -> bool:
        """The node is ticking but its owner work waits for Run or Resume.

        Either the owner refused to start it under a Stop (held before
        admission), or an ordinary operator Stop interrupted it and it waits
        for Resume. A Stop fault needing recovery is not presented as held.
        """
        return (self.status is py_trees.common.Status.RUNNING
                and (self._held_before_admission is not None
                     or self._held_after_operator_stop))


class _TaskPhase(Enum):
    """Where one owner-task attempt is; every transition is in this class."""

    SUBMIT = "submit"  # No owner request yet (or held before admission).
    OBSERVE = "observe"  # The accepted task is observed until it settles.
    HOLDING = "holding"  # An authored pause holds the task for the operator.
    CLOSING = "closing"  # The failed task is being closed before its result.
    BACKOFF = "backoff"  # Motion: closed; the automatic retry waits.
    DONE = "done"  # The result was returned to the tree.


# The participant recovery assessment's `execution_state`: Core's closed
# ExecutionState vocabulary, exactly as core::to_string spells it (TitleCase).
# Distinct from the lowercase outer operation.state vocabulary
# (FAILED_/INTERRUPTED_EXECUTION_STATES); any other spelling is unqualified.
_PARTICIPANT_EXECUTION_STATES: Final = frozenset({
    "Idle", "Planning", "Validating", "Executing", "Interrupted",
    "Stopping", "Stopped", "Completed", "Failed", "InterventionRequired",
})
_PARTICIPANT_FAILED_STATES: Final = frozenset({"Failed", "InterventionRequired"})
_PARTICIPANT_INTERRUPTED_STATES: Final = frozenset({"Stopped", "Interrupted"})


def _participant_execution_state(assessment: Mapping[str, Any]) -> str | None:
    """The assessment's exact Core execution state, or None if unknown."""

    state = assessment.get("execution_state")
    return state if isinstance(state, str) and state in _PARTICIPANT_EXECUTION_STATES else None


# Assessment statuses that decide an execution (not waiting or unavailable).
_SETTLED_ASSESSMENT_STATUSES: Final = frozenset({"retry_ready", "action_required", "reset_required"})


def _current_assessment(task: OwnerTaskSnapshot) -> Mapping[str, Any] | None:
    """The assessment of this terminal execution, or None.

    A present predecessor (an earlier execution's assessment, still cached
    while the failure refreshes) or one still describing an operation in
    flight does not qualify: it is not the failed or interrupted execution
    of this attempt.
    """

    assessment = task.metadata.get("recovery_assessment")
    if not isinstance(assessment, Mapping):
        return None
    eligibility = assessment.get("retry_eligibility")
    if isinstance(eligibility, Mapping) and eligibility.get("reason") == "operation_in_flight":
        return None
    execution_states = (
        _PARTICIPANT_FAILED_STATES
        if task.state is OwnerTaskState.FAILED
        else _PARTICIPANT_INTERRUPTED_STATES
    )
    return assessment if _participant_execution_state(assessment) in execution_states else None


def _settled_assessment(task: OwnerTaskSnapshot) -> bool:
    """The current assessment is settled (its status decides an execution)."""

    assessment = _current_assessment(task)
    return assessment is not None and assessment.get("status") in _SETTLED_ASSESSMENT_STATUSES


def _close_eligible(task: OwnerTaskSnapshot) -> bool:
    """The Kernel's own close permission (DX-1169, SX-1972..1974) on the exact
    current assessment of this terminal execution.

    It is read first and alone: retry eligibility, the legacy assessment
    status (reset_required, intervention_required), the execution state's
    own label and `allowed_actions` never veto or replace it. The port reads
    only the assessment of the observed operation revision.
    """

    assessment = _current_assessment(task)
    return assessment is not None and assessment.get("eligible_close") is True


def _needs_intervention(task: OwnerTaskSnapshot) -> bool:
    """Without close permission, the operation shows uncertain effects or an
    unresolved intervention: its identity is retained for the existing
    intervention/containment handling, never waited on (SX-1972/1973)."""

    if task.metadata.get("operation_state") == "intervention_required":
        return True
    if output_may_have_acted(task.metadata.get("output_evidence")):
        # Contract 53: unknown source acceptance is an unknown effect.
        return True
    assessment = task.metadata.get("recovery_assessment")
    if isinstance(assessment, Mapping) and (
        _participant_execution_state(assessment) == "InterventionRequired"
    ):
        return True
    fact = task.metadata.get("operation_failure")
    terminal_facts = fact.get("terminal_facts") if isinstance(fact, Mapping) else None
    return isinstance(terminal_facts, (list, tuple)) and any(
        isinstance(item, Mapping) and item.get("effect") == "effect_uncertain"
        for item in terminal_facts
    )


def _retired(task: OwnerTaskSnapshot) -> bool:
    """An exact successful read shows the operation no longer resident: its
    retirement already completed (SX-1974). A failed read is not this proof."""

    return task.metadata.get("resident") is False


def _close_decidable(task: OwnerTaskSnapshot) -> bool:
    """This read decides the failed operation's closure (close, already
    retired, or retained); otherwise its assessment is not yet available."""

    return (
        _close_eligible(task)
        or _retired(task)
        or _settled_assessment(task)
        or _needs_intervention(task)
    )


# Exact rereads (one per tick) of a normal failed resident whose current
# assessment stays unavailable, before its identity is retained for the
# existing intervention handling instead (SX-1972..1974): never an
# indefinite RUNNING wait.
_ASSESSMENT_REREAD_LIMIT: Final = 100


def _is_stop_pause(task: OwnerTaskSnapshot) -> bool:
    """A Stop holds the motion for the operator's decision: not a final failure."""

    return task.metadata.get("authored_failure_policy_parameter") == "operator_stop_policy"


class OwnerTaskAdapter(_OwnerAdapter):
    """Submit once, observe owner state, and leave nothing resident.

    Exact success is acknowledged. A failure that returns to the tree closes
    its settled operation first (`close_operation`); a failure the node's own
    recovery policy holds keeps it live for the operator's owner actions. An
    attempt the tree abandons (INVALID) is drained: cancelled, then closed
    once its Stop settled. A refused or impossible close leaves the operation
    retained for Reset, as before.
    """

    def __init__(
        self,
        *,
        port: OwnerTaskPort,
        owner_port_name: str = "unspecified",
        intervention_port: ProgramInterventionPort | None = None,
        require_initial_running: bool = False,
        operator_stop_state: OperatorMotionStopState | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self._port = port
        self._owner_port_name = owner_port_name
        self._intervention_port = intervention_port
        self._require_initial_running = require_initial_running
        self._operator_stop_state = operator_stop_state
        self._task: OwnerTaskSnapshot | None = None
        self._phase = _TaskPhase.SUBMIT
        # An abandoned attempt's task still to close, with its identity.
        self._drain: tuple[OwnerTaskSnapshot, NodeAttemptIdentity] | None = None
        self._drain_rereads = 0
        # Exact rereads of this attempt's unavailable failure assessment.
        self._assessment_rereads = 0
        self._holds_failure = False
        # The failed operation's own facts when its failure reached the tree.
        self._operation_failure: Mapping[str, Any] | None = None

    def hold_failures_for_intervention(self) -> None:
        """This node's own recovery policy holds its failures: the failed
        operation stays live for the operator instead of closing."""

        self._holds_failure = True

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._task = None
        self._phase = _TaskPhase.SUBMIT
        self._operation_failure = None
        self._assessment_rereads = 0

    @property
    def operation_failure(self) -> Mapping[str, Any] | None:
        """The failed operation's Kernel facts, once its failure reached the
        tree (the run's `operation_failures` row); None otherwise."""

        return self._operation_failure

    @property
    def owner_recovery_context(self) -> Mapping[str, Any] | None:
        if self._task is None:
            return None
        assessment = self._task.metadata.get("recovery_assessment")
        return {
            "owner_port_name": self._owner_port_name,
            "owner_task_id": self._task.task_id,
            "owner_assessment": (assessment if isinstance(assessment, Mapping) else None),
        }

    def _closable(self, task: OwnerTaskSnapshot | None = None) -> bool:
        """The port closes operations, and (when given) this task is one the
        Kernel admitted; an unknown admission stays for Reset."""
        return callable(getattr(self._port, "close", None)) and (
            task is None or isinstance(task.metadata.get("operation_id"), str)
        )

    def _close_request_id(self, identity: NodeAttemptIdentity) -> str:
        return request_identity("program-close", identity.program_run_id, identity.node_attempt_id)

    def _await_settled_assessment(
        self, task: OwnerTaskSnapshot
    ) -> tuple[py_trees.common.Status | None, OwnerTaskSnapshot]:
        """A failure can arrive before its assessment is readable. Reread the
        exact current operation, read-only, once per tick: (RUNNING, task)
        while it is not yet decidable, (FAILURE, task) for a malformed read,
        and (None, task) once the read decides its closure (`_close_decidable`)
        or the bounded rereads are spent (then it is retained)."""

        if _close_decidable(task):
            return None, task
        try:
            observed = self._port.observe(task.task_id)
        except OwnerTaskObservationUnavailableError:
            return self._reread_pending(task)
        except Exception as exc:
            self._record_owner_error("owner_request_failed", exc)
            return py_trees.common.Status.FAILURE, task
        if observed.task_id != task.task_id:
            self._record_owner_failure(
                "owner_task_identity_changed",
                "owner observation selected another task",
            )
            return py_trees.common.Status.FAILURE, task
        self._task = observed
        read_failure = observed.metadata.get("recovery_assessment_failure")
        if isinstance(read_failure, Mapping):
            # A malformed or unexpected read is not temporary absence.
            self._record_owner_failure(
                str(read_failure.get("reason_code") or "owner_recovery_assessment_invalid"),
                str(read_failure.get("message") or "recovery assessment is invalid"),
            )
            return py_trees.common.Status.FAILURE, observed
        if _close_decidable(observed):
            return None, observed
        return self._reread_pending(observed)

    def _reread_pending(
        self, task: OwnerTaskSnapshot
    ) -> tuple[py_trees.common.Status | None, OwnerTaskSnapshot]:
        self._assessment_rereads += 1
        if self._assessment_rereads < _ASSESSMENT_REREAD_LIMIT:
            return py_trees.common.Status.RUNNING, task
        return None, task  # Still unavailable: retained, never waited on.

    def _note_operation_failure(self, task: OwnerTaskSnapshot) -> None:
        fact = task.metadata.get("operation_failure")
        assessment = task.metadata.get("recovery_assessment")
        recovery_class = (
            assessment.get("recovery_class") if isinstance(assessment, Mapping) else None
        )
        self._operation_failure = (
            {**fact, "recovery_class": recovery_class}
            if isinstance(fact, Mapping) and isinstance(recovery_class, str) and recovery_class
            else None
        )

    def _fail_to_tree(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        """The failure leaves this node: close its operation first, unless the
        node's own recovery policy holds it live for the operator."""

        self._record_owner_failure(task.reason_code, task.message, task.details)
        self._note_operation_failure(task)
        if self._holds_failure or not self._closable(task):
            self._phase = _TaskPhase.DONE
            return py_trees.common.Status.FAILURE
        self._phase = _TaskPhase.CLOSING
        return self._advance_close(task)

    def _advance_close(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        """Close the failed operation, then continue (`_after_close`).

        The exact current close permission decides first. Without it, an
        operation already retired (resident false) leaves nothing to close;
        otherwise (no permission, uncertain effects, an unresolved
        intervention, a refused or impossible close, or an assessment still
        unavailable after the bounded rereads) the operation stays retained
        (Reset releases it; its alarm stands) and the failure is returned.
        An unknown close outcome keeps the node RUNNING; the same request
        replays.
        """

        pending, task = self._await_settled_assessment(task)
        if pending is py_trees.common.Status.RUNNING:
            return pending
        if pending is None and not _close_eligible(task) and _retired(task):
            # Retirement already completed: nothing is left to close.
            self._task = None
            return self._after_close(closed=True)
        if pending is not None or self._identity is None or not _close_eligible(task):
            return self._after_close(closed=False)
        try:
            closed = self._port.close(
                task.task_id, self._identity, request_id=self._close_request_id(self._identity)
            )
        except OwnerTaskSettlementPendingError:
            return py_trees.common.Status.RUNNING
        except Exception:
            closed = False
        if closed:
            self._task = None
        return self._after_close(closed=closed is True)

    def _after_close(self, *, closed: bool) -> py_trees.common.Status:
        self._phase = _TaskPhase.DONE
        return py_trees.common.Status.FAILURE

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        if self._phase is _TaskPhase.CLOSING:
            return self._advance_close(task)
        holding = self._phase is _TaskPhase.HOLDING
        assessment = task.metadata.get("recovery_assessment")
        operator_stop = (
            task.state is OwnerTaskState.INTERRUPTED
            and isinstance(assessment, Mapping)
            and assessment.get("recovery_class") in PROGRAM_MOTION_STOP_RECOVERY_CLASSES
        )
        self._held_after_operator_stop = is_ordinary_operator_stop(task)
        if operator_stop and self._operator_stop_state is not None:
            if (
                not isinstance(task.metadata.get("active_stop_id"), str)
                or not task.metadata["active_stop_id"]
                or type(task.metadata.get("active_stop_generation")) is not int
                or task.metadata["active_stop_generation"] < 1
            ):
                self._record_owner_failure(
                    "owner_stop_identity_missing",
                    "ordinary operator Stop omitted its exact Stop identity",
                )
                return py_trees.common.Status.FAILURE
            if not holding:
                self._operator_stop_state.refresh()
        if holding:
            if self._intervention_port is None or self._identity is None:
                self._record_node_failure(
                    "program_intervention_port_unavailable",
                    "held recovery lost its exact owner identity",
                )
                return py_trees.common.Status.FAILURE
            try:
                resolution = self._intervention_port.consume_resolution(self._identity)
            except Exception as exc:
                self._record_node_failure("program_recovery_resolution_failed", str(exc))
                return py_trees.common.Status.FAILURE
            if resolution is None:
                return py_trees.common.Status.RUNNING
            action, result = resolution
            if action == "retry":
                # A fresh attempt; the already-submitted owner request is
                # never changed. The operator released this node's hold.
                self._begin_attempt(restart_series=True)
                return py_trees.common.Status.RUNNING
            self._phase = _TaskPhase.DONE
            if action == "manual_success":
                return (
                    py_trees.common.Status.SUCCESS
                    if self._write_output(
                        result,
                        owner_metadata={"result_source": "manual_recovery"},
                        adapter_metadata={
                            "program_run_id": self._identity.program_run_id,
                            "node_attempt_id": self._identity.node_attempt_id,
                            "owner_task_id": task.task_id,
                        },
                    )
                    else py_trees.common.Status.FAILURE
                )
            self._record_node_failure(
                "manual_recovery_failure", "operator staged a failed result"
            )
            return py_trees.common.Status.FAILURE
        if task.state is OwnerTaskState.ACTIVE:
            return py_trees.common.Status.RUNNING
        if task.state is OwnerTaskState.SUCCEEDED or (
            task.state is OwnerTaskState.FAILED
            and task.settlement_proven
            and task.metadata.get("authored_failure_policy") in (None, "condition_failure")
        ):
            if self._phase is not _TaskPhase.DONE:
                if self._identity is None:
                    self._record_node_failure(
                        "owner_result_invalid",
                        "node-attempt identity is absent",
                    )
                    return py_trees.common.Status.FAILURE
                try:
                    self._port.acknowledge(task.task_id, self._identity)
                except OwnerTaskSettlementPendingError:
                    return py_trees.common.Status.RUNNING
                except Exception as exc:
                    self._record_owner_error("owner_acknowledgement_failed", exc)
                    return py_trees.common.Status.FAILURE
                self._phase = _TaskPhase.DONE
        if task.state is OwnerTaskState.SUCCEEDED:
            return (
                py_trees.common.Status.SUCCESS
                if self._write_output(
                    task.output,
                    owner_metadata=task.metadata,
                    adapter_metadata={
                        "program_run_id": self._identity.program_run_id,
                        "node_attempt_id": self._identity.node_attempt_id,
                        "owner_task_id": task.task_id,
                    },
                )
                else py_trees.common.Status.FAILURE
            )
        if self._phase is _TaskPhase.DONE:
            # A settled final failure, acknowledged above: nothing to close.
            self._record_owner_failure(task.reason_code, task.message, task.details)
            return py_trees.common.Status.FAILURE
        policy = task.metadata.get("authored_failure_policy")
        if policy is not None:
            if policy not in {"pause_for_operator", "condition_failure"}:
                self._record_owner_failure(
                    "owner_result_invalid",
                    "owner returned an unknown authored failure policy",
                )
                return py_trees.common.Status.FAILURE
            if policy == "pause_for_operator":
                if self._intervention_port is None or self._identity is None:
                    self._record_node_failure(
                        "program_intervention_port_unavailable",
                        "pause_for_operator requires the production intervention port",
                    )
                    return py_trees.common.Status.FAILURE
                parameter = task.metadata.get("authored_failure_policy_parameter")
                if not isinstance(parameter, str) or not parameter:
                    self._record_owner_failure(
                        "owner_result_invalid",
                        "pause_for_operator has no authored policy parameter",
                    )
                    return py_trees.common.Status.FAILURE
                # An operator Stop's interruption is not a failure; any
                # other paused owner result is the owner's failure.
                stop_interruption = parameter == "operator_stop_policy"
                try:
                    self._intervention_port.require(
                        self._identity,
                        reason_code=(task.reason_code or "operator_intervention_required"),
                        message=task.message,
                        policy_parameter=parameter,
                        details=None if stop_interruption else task.details,
                        origin=(
                            None
                            if stop_interruption
                            else self._owner_failure.for_node(
                                self._identity.node_instance_id
                            )
                        ),
                        owner_port_name=self._owner_port_name,
                        owner_task_id=task.task_id,
                        owner_assessment=(
                            task.metadata.get("recovery_assessment")
                            if isinstance(
                                task.metadata.get("recovery_assessment"),
                                Mapping,
                            )
                            else None
                        ),
                    )
                except Exception as exc:
                    self._record_node_failure(
                        "program_intervention_record_failed",
                        str(exc),
                    )
                    return py_trees.common.Status.FAILURE
                self._phase = _TaskPhase.HOLDING
                if not stop_interruption:
                    self._note_operation_failure(task)
                if self._operator_stop_state is not None:
                    self._operator_stop_state.note_task_observation_change()
                self._record_owner_failure(task.reason_code, task.message, task.details)
                # Keep this exact terminal owner task in place.  The production
                # scheduler removes the run from automatic ticking, so neither
                # a retry nor a stale output write can occur.
                return py_trees.common.Status.RUNNING
        return self._fail_to_tree(task)

    def _on_priority_refusal(
        self, error: OwnerInvocationRefusedByPriorityError
    ) -> py_trees.common.Status:
        self._record_owner_error("owner_request_failed", error)
        return py_trees.common.Status.FAILURE

    def _finish(self, status: py_trees.common.Status) -> py_trees.common.Status:
        """Hook on every status this node returns (motion writes its outcome)."""

        return status

    def update(self) -> py_trees.common.Status:
        return self._finish(self._update())

    def _update(self) -> py_trees.common.Status:
        if self._drain is not None and not self.drain_invalidated():
            # The abandoned attempt's operation still settles; a new Execute
            # on that robot would be refused while it stays resident.
            return py_trees.common.Status.RUNNING
        if self._wait_for_admission_release():
            return py_trees.common.Status.RUNNING
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        if self._phase is _TaskPhase.SUBMIT:
            gate = self._mock_simulation_ready()
            if gate is not None:
                return gate
        try:
            if self._phase is _TaskPhase.SUBMIT:
                self._task = self._port.start(
                    self._node_id,
                    self._identity,
                    self._owner_inputs,
                )
                self._phase = _TaskPhase.OBSERVE
                if self._require_initial_running:
                    return py_trees.common.Status.RUNNING
            elif self._task is not None and self._task.state is OwnerTaskState.ACTIVE:
                try:
                    observed = self._port.observe(self._task.task_id)
                except OwnerTaskObservationUnavailableError:
                    # Keep the accepted snapshot and retry only its exact task
                    # identity on a later tick. Submission and acknowledgement
                    # remain one-shot.
                    return py_trees.common.Status.RUNNING
                if observed.task_id != self._task.task_id:
                    self._record_owner_failure(
                        "owner_task_identity_changed",
                        "owner observation selected another task",
                    )
                    return py_trees.common.Status.FAILURE
                if observed.state is not self._task.state and self._operator_stop_state is not None:
                    self._operator_stop_state.note_task_observation_change()
                self._task = observed
        except OwnerInvocationHeldError as error:
            self._task = None
            self._phase = _TaskPhase.SUBMIT
            self._hold_before_admission(error)
            return py_trees.common.Status.RUNNING
        except OwnerInvocationRefusedByPriorityError as error:
            self._task = None
            self._phase = _TaskPhase.DONE
            return self._on_priority_refusal(error)
        except Exception as exc:
            self._record_owner_error("owner_request_failed", exc)
            return py_trees.common.Status.FAILURE
        if self._task is None:
            return py_trees.common.Status.FAILURE
        return self._settle(self._task)

    def observe_while_operator_stop_held(self) -> None:
        """Refresh only an already accepted task while its subtree is held."""

        if self._task is None or self._identity is None or self.status in {
            py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE}:
            return
        try:
            if self._task.state is OwnerTaskState.ACTIVE:
                observed = self._port.observe(self._task.task_id)
                if observed.task_id != self._task.task_id:
                    self._record_owner_failure(
                        "owner_task_identity_changed",
                        "owner observation selected another task",
                    )
                    return
                if observed.state is not self._task.state and self._operator_stop_state is not None:
                    self._operator_stop_state.note_task_observation_change()
                self._task = observed
            status = self._finish(self._settle(self._task))
            if status is not py_trees.common.Status.RUNNING:
                self.stop(status)
        except OwnerTaskObservationUnavailableError:
            return
        except OwnerInvocationHeldError as error:
            self._task = None
            self._phase = _TaskPhase.SUBMIT
            self._hold_before_admission(error)
        except Exception as exc:
            self._record_owner_error("owner_request_failed", exc)

    def reconcile_full_stop_task(self, *, selected_resume: bool = False) -> str:
        """Observe accepted Program-scope work without ticking the tree.

        Full Stop observes only physical tasks in its existing scope. Selected
        Resume also waits for other accepted owners' work; it does not cancel
        them or pretend the Kernel stopped them.
        """

        if not selected_resume and self._owner_port_name != "kernel_operations":
            return "unaffected"
        if self._drain is not None:
            self.drain_invalidated()
        if self._task is None:
            return self._control_state(None)
        self.observe_while_operator_stop_held()
        return self._control_state(self._task)

    @property
    def has_active_task(self) -> bool:
        return self._control_state(self._task) == "active"

    def observe_control_state(self) -> str:
        """Read status without settling tasks, acknowledging or writing outputs."""
        task = self._task
        if task is not None and task.state is OwnerTaskState.ACTIVE:
            try:
                observed = self._port.observe(task.task_id)
            except OwnerInvocationHeldError as error:
                self._task = None
                self._phase = _TaskPhase.SUBMIT
                self._hold_before_admission(error)
                return self._control_state(None)
            if observed.task_id != task.task_id:
                raise ValueError("owner observation selected another task")
            task = observed
        return self._control_state(task)

    def cached_control_state(self) -> str:
        """Project the last retained owner fact without performing owner I/O."""

        return self._control_state(self._task)

    def _control_state(self, task: OwnerTaskSnapshot | None) -> str:
        if self._drain is not None:
            # An abandoned attempt's operation is still being drained.
            return "active"
        if task is None:
            return "inactive"
        if task.state is OwnerTaskState.ACTIVE:
            return "active"
        acknowledged = self._phase is _TaskPhase.DONE
        if (task.state is OwnerTaskState.SUCCEEDED or task.settlement_proven) and not acknowledged:
            # The action result alone cannot release a selected hold while
            # its existing owner-settlement acknowledgement is still pending.
            return "unrecoverable" if self._failure_reason_code else "active"
        assessment = task.metadata.get("recovery_assessment")
        if (
            task.state is OwnerTaskState.INTERRUPTED
            and isinstance(assessment, Mapping)
            and assessment.get("recovery_class") in PROGRAM_MOTION_STOP_RECOVERY_CLASSES
        ):
            return "held"
        if task.state is OwnerTaskState.SUCCEEDED or (
            task.state is OwnerTaskState.FAILED and task.settlement_proven
        ):
            return "settled"
        return "unrecoverable"

    @property
    def has_pending_drain(self) -> bool:
        return self._drain is not None

    def drain_invalidated(self) -> bool:
        """Close the operation of an attempt the tree abandoned (INVALID),
        once its Stop settled. True when nothing is left to drain.

        Success is acknowledged as usual; an operation that is not eligible
        to close (a fault, unknown effects) stays retained for Reset.
        """

        if self._drain is None:
            return True
        task, identity = self._drain
        try:
            observed = self._port.observe(task.task_id)
        except OwnerTaskObservationUnavailableError:
            return self._drain_pending(task, identity)
        except Exception:
            # No longer retained (an accepted recovery or Reset retired it).
            self._drain = None
            return True
        if observed.state is OwnerTaskState.ACTIVE:
            self._drain = (observed, identity)
            return False
        try:
            if observed.state is OwnerTaskState.SUCCEEDED or observed.settlement_proven:
                self._port.acknowledge(observed.task_id, identity)
            elif _close_eligible(observed):
                self._port.close(
                    observed.task_id, identity, request_id=self._close_request_id(identity)
                )
            elif not _close_decidable(observed):
                return self._drain_pending(observed, identity)
        except OwnerTaskSettlementPendingError:
            self._drain = (observed, identity)
            return False
        except Exception:
            pass  # Refused or impossible: retained for Reset, as before.
        self._drain = None
        return True

    def _drain_pending(self, task: OwnerTaskSnapshot, identity: NodeAttemptIdentity) -> bool:
        """The drained operation's assessment is still unavailable: reread it
        on later ticks, then leave it retained for Reset (bounded)."""

        self._drain_rereads += 1
        if self._drain_rereads < _ASSESSMENT_REREAD_LIMIT:
            self._drain = (task, identity)
            return False
        self._drain = None
        return True

    def terminate(self, new_status: py_trees.common.Status) -> None:
        # Halt, Stop or cancel of a running attempt: a cancellable node
        # reports `interrupted`, including a held or not yet admitted one,
        # once the stop is confirmed.
        self._record_interruption(new_status)
        if new_status is not py_trees.common.Status.INVALID:
            return
        task, identity = self._task, self._identity
        if (
            task is None
            or identity is None
            or self._phase in {_TaskPhase.DONE, _TaskPhase.BACKOFF}
        ):
            return
        if task.state is OwnerTaskState.ACTIVE:
            try:
                self._port.cancel(task.task_id, identity)
            except Exception as exc:
                # The owner refused the cancellation: the stop is not
                # confirmed, so the attempt reports `failure`, never
                # `interrupted`, and the run pauses for operator attention.
                self._record_stop_failure()
                self._record_owner_error("owner_cancellation_failed", exc)
                return
        if self._closable(task):
            self._drain = (task, identity)
            self._drain_rereads = 0
            self._phase = _TaskPhase.DONE


@dataclass(frozen=True, slots=True)
class _PendingRetry:
    cause: str
    delay_s: float
    budget: int
    code: str | None
    message: str | None


_PREEMPTION_OUTCOMES: Final = {"preempted": "preempted", "superseded_unsent": "superseded"}


class MotionTaskAdapter(OwnerTaskAdapter):
    """A motion node retries its own eligible failed attempt (spec 400 §7).

    The Kernel alone decides eligibility. On a failure this adapter decides
    first: an eligible attempt with budget left is closed, waits on the
    Program clock, passes the owner's motion-attention check and submits a
    fresh Execute; otherwise the failure is held or closed as for any owner
    task. Any operator release of a hold over the node restarts the budget.
    A logically preempted (or priority-refused) node ends with its literal
    `preempted_result`; every end writes the outcome to the node's
    system-owned key `node_outcome.<instance_id>` (contract item 42).
    """

    def __init__(
        self,
        *,
        program_clock: ProgramClock,
        operator_stop_hold_scope: str | None = None,
        motion_attention: Callable[[str, str], None] | None = None,
        audit_sink: Callable[[dict[str, Any]], None] | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self._motion_attention = motion_attention
        self._audit_sink = audit_sink
        self._retry_time = AttemptTime(
            program_clock,
            operator_stop_state=(
                self._operator_stop_state if operator_stop_hold_scope is not None else None
            ),
            operator_stop_hold_scope=operator_stop_hold_scope,
        )
        self._retries_used = 0
        self._retry: _PendingRetry | None = None
        self._outcome: str | None = None
        self._outcome_written = False
        self._preempted = False

    @property
    def preempted(self) -> bool:
        """This attempt was logically preempted, superseded unsent or refused
        by priority: its `preempted_result` goes straight to the tree, never held for
        intervention nor retried automatically (maintainer decision)."""

        return self._preempted

    def _begin_attempt(self, *, restart_series: bool) -> None:
        if restart_series:
            self._retries_used = 0
        self._retry = None
        self._outcome = None
        self._outcome_written = False
        self._preempted = False
        super()._begin_attempt(restart_series=restart_series)

    def restart_retry_series(self) -> None:
        """The operator released a hold over this node: full budget again."""

        self._retries_used = 0

    def _update(self) -> py_trees.common.Status:
        if self._phase is not _TaskPhase.BACKOFF:
            return super()._update()
        retry = self._retry
        identity = self._identity
        assert retry is not None and identity is not None  # Set with BACKOFF.
        if self._retry_time.elapsed() < retry.delay_s:
            return py_trees.common.Status.RUNNING
        request_id = request_identity(
            "program-automatic-retry", identity.program_run_id, identity.node_attempt_id
        )
        try:
            if self._motion_attention is None:
                raise RuntimeError("automatic motion retry has no motion attention check")
            # The check an operator Retry runs before any motion effect. The
            # Kernel's refusal keeps its own code.
            self._motion_attention(identity.node_instance_id, request_id)
        except Exception as error:
            self._phase = _TaskPhase.DONE
            self._record_owner_failure(
                str(getattr(error, "code", None) or "motion_attention_blocked"),
                str(error) or "operator-alarm attention blocks this motion",
            )
            return py_trees.common.Status.FAILURE
        self._begin_attempt(restart_series=False)
        return super()._update()

    def observe_while_operator_stop_held(self) -> None:
        # A held node waits: no fresh attempt starts under a Stop.
        if self._phase is not _TaskPhase.BACKOFF:
            super().observe_while_operator_stop_held()

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        preemption = task.metadata.get("preemption")
        if isinstance(preemption, Mapping) and self._phase is _TaskPhase.OBSERVE:
            return self._end_preempted(task, preemption)
        if (
            self._phase is _TaskPhase.OBSERVE
            and task.state in {OwnerTaskState.FAILED, OwnerTaskState.INTERRUPTED}
            and not is_ordinary_operator_stop(task)
            and not _is_stop_pause(task)
            # An unknown admission is no operation to decide on: Reset owns it.
            and (self._closable(task) or (self._retry_limit() > 0 and not self._closable()))
        ):
            # Decide on a coherent assessment only: retry, hold or close.
            pending, task = self._await_settled_assessment(task)
            if pending is not None:
                return pending
            retry = self._retry_for(task)
            if retry is not None:
                self._retry = retry
                self._phase = _TaskPhase.CLOSING
                return self._advance_close(task)
        return super()._settle(task)

    def _after_close(self, *, closed: bool) -> py_trees.common.Status:
        retry, self._retry = self._retry, None
        if retry is None or not closed:
            if retry is not None and self._task is not None:
                # The retry needed the close: this failure returns instead,
                # with its operation retained.
                self._record_owner_failure(
                    self._task.reason_code, self._task.message, self._task.details
                )
                self._note_operation_failure(self._task)
            return super()._after_close(closed=closed)
        self._retries_used += 1
        self._retry = retry
        self._phase = _TaskPhase.BACKOFF
        self._retry_time.start(exclude_held=True)
        attempt = self._retries_used + 1
        if self._audit_sink is not None:
            # The failed attempt's own Kernel failure, as observed; null when absent.
            self._audit_sink(
                {
                    "kind": "program_node_automatic_retry",
                    "node_instance_id": self._node_instance_id,
                    "attempt": attempt,
                    "max_attempts": retry.budget + 1,
                    "cause": retry.cause,
                    "delay_s": retry.delay_s,
                    "code": retry.code,
                    "message": retry.message,
                }
            )
        self.feedback_message = (
            f"automatic retry {attempt} of {retry.budget + 1} after {retry.cause}"
        )
        return py_trees.common.Status.RUNNING

    def _retry_limit(self) -> int:
        """This attempt's automatic retry budget (spec 400 §7, spec 410 §4.1).

        The item-7 grammar applies to the attempt's resolved parameters: a
        relative joint move, or a relative pose/path without a target
        reference, is never retried. Otherwise the literal budget.
        """

        inputs = self._inputs
        if inputs is None:
            return 0
        if inputs.get("target_mode", "absolute") == "relative" and (
            self._node_id == "motion.move_to_joint_target"
            or inputs.get("target_reference") is None
        ):
            # A joint delta, or an offset from the robot's own pose, would be
            # applied again by a re-read.
            return 0
        budget = inputs.get("automatic_retries", _DEFAULT_AUTOMATIC_RETRIES)
        return budget if type(budget) is int and 0 <= budget <= 10 else 0

    def _retry_for(self, task: OwnerTaskSnapshot) -> _PendingRetry | None:
        """The retry this settled failed attempt earns, or None."""

        limit = self._retry_limit()
        inputs = self._inputs
        if (
            self._identity is None
            or inputs is None
            or limit == 0
            or self._retries_used >= limit
            or not _close_eligible(task)
        ):
            return None
        delay = inputs.get("automatic_retry_delay_s", _DEFAULT_AUTOMATIC_RETRY_DELAY_S)
        if (
            type(delay) not in (int, float)
            or not math.isfinite(delay)
            or not 0 <= delay <= 60
        ):
            return None
        assessment = task.metadata.get("recovery_assessment")
        eligibility = (
            assessment.get("retry_eligibility") if isinstance(assessment, Mapping) else None
        )
        if not isinstance(eligibility, Mapping) or eligibility.get("eligible") is not True:
            return None
        cause = eligibility.get("cause")
        if cause not in _AUTOMATIC_RETRY_CAUSES:
            return None
        return _PendingRetry(
            cause, float(delay), limit, task.reason_code or None, task.message[:1024] or None
        )

    def _preempted_result(self, reason_code: str, message: str) -> py_trees.common.Status:
        self._phase = _TaskPhase.DONE
        if (self._inputs or {}).get("preempted_result", "failure") == "success":
            return py_trees.common.Status.SUCCESS
        self._record_owner_failure(reason_code, message)
        return py_trees.common.Status.FAILURE

    def _end_preempted(
        self, task: OwnerTaskSnapshot, preemption: Mapping[str, Any]
    ) -> py_trees.common.Status:
        """Logically preempted at the newcomer's acceptance: the node ends now;
        the Kernel owns the physical handover and retires the operation."""

        self._outcome = _PREEMPTION_OUTCOMES.get(str(preemption.get("kind")), "preempted")
        release = getattr(self._port, "release_preempted", None)
        if callable(release) and self._identity is not None:
            try:
                release(task.task_id, self._identity)
            except Exception as error:
                self._record_owner_error("owner_request_failed", error)
                self._phase = _TaskPhase.DONE
                return py_trees.common.Status.FAILURE
        self._task = None
        by = preemption.get("by") if isinstance(preemption.get("by"), Mapping) else {}
        return self._end_by_priority(
            by,
            f"motion_{self._outcome}",
            f"motion {self._outcome} by node {by.get('node_instance_id', 'unknown')}",
        )

    def _on_priority_refusal(
        self, error: OwnerInvocationRefusedByPriorityError
    ) -> py_trees.common.Status:
        """Refused at Execute (no effect started): ends like a preempted node."""

        self._outcome = "refused_by_priority"
        return self._end_by_priority(
            error.winner,
            "refused_by_priority",
            f"refused by the higher-priority motion of node "
            f"{error.winner.get('node_instance_id') or 'unknown'}",
        )

    def _end_by_priority(
        self, by: Mapping[str, Any], reason_code: str, message: str
    ) -> py_trees.common.Status:
        """Its literal `preempted_result` goes straight to the tree (no hold,
        no automatic retry); history records the outcome either way."""

        self._preempted = True
        status = self._preempted_result(reason_code, message)
        if self._audit_sink is not None:
            self._audit_sink(
                {
                    "kind": "program_node_preempted",
                    "node_instance_id": self._node_instance_id,
                    "outcome": self._outcome,
                    "by_node_instance_id": by.get("node_instance_id"),
                    "by_operation_id": by.get("operation_id"),
                    "result": status.name.lower(),
                }
            )
        return status

    def _finish(self, status: py_trees.common.Status) -> py_trees.common.Status:
        """Write the outcome to the node's system-owned key before its result."""

        if (
            status not in {py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE}
            or self._outcome_written
        ):
            return status
        self._outcome_written = True
        if self._identity is None:
            return status
        outcome = self._outcome or (
            "completed" if status is py_trees.common.Status.SUCCESS else "failed"
        )
        try:
            self._input_resolver.value_store.write_node_outcome(
                self._node_instance_id,
                outcome,
                metadata={
                    "node_id": self._node_id,
                    "node_instance_id": self._node_instance_id,
                    "program_run_id": self._identity.program_run_id,
                    "node_attempt_id": self._identity.node_attempt_id,
                },
            )
        except (KeyError, ProgramValueError, TypeError) as exc:
            self._record_owner_failure("owner_output_rejected", str(exc))
            return py_trees.common.Status.FAILURE
        return status


class InterruptMotionTaskAdapter(MotionTaskAdapter):
    """`motion.move_until_signal` / `motion.probe_search` (P5, I5).

    The terminal operation's typed rule result (`metadata["interrupt_operation"]`,
    the operation projection with its participants' `io_rule_outcomes`) is
    mapped by `interrupt_outcome.map_interrupt_result` to the node result, the
    node outcome (`signal_trip` / `no_signal_trip` / `failed`) and one history
    row. The containing Stop stage, needed only for an escalated trip, comes
    from the robot's qualified Stop chain (`interrupt_stop_chain`, per
    participant: the Targets motion command's `stop_response` stages of the
    binding the operation used) and the detailed Stop terminal
    projection of the row's Stop and certificate (`interrupt_stop_terminal`,
    the `stop_terminal_projected` row the Runner's existing events reader
    retains; `contained_stages_from_stop_terminal`, per participant); when either is
    absent the stage is unproven and the node fails. An ordinary
    Stop (no interrupt terminal) keeps the existing motion handling.
    """

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._interrupt: InterruptNodeResult | None = None
        self._stop_terminal_waiting = False

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._interrupt = None
        self._stop_terminal_waiting = False

    def _retry_limit(self) -> int:
        # Search and contact moves have no automatic retry (spec 410 §4.1);
        # a source loss (D2) is never retried either (contract 46c).
        return 0

    def _await_stop_terminal(
        self, task: OwnerTaskSnapshot
    ) -> tuple[py_trees.common.Status | None, OwnerTaskSnapshot]:
        """SX-2222/2226: the Stop terminal record may follow the operation's
        terminal state. While the port reports it pending, the node stays
        RUNNING and re-observes the terminal task once per later tick (one
        read_stop each); the wait ends with the record or unproven evidence
        (Failure), or when the node is halted."""

        if task.metadata.get("interrupt_stop_terminal_pending") is not True:
            return None, task
        if self._stop_terminal_waiting:
            try:
                observed = self._port.observe(task.task_id)
            except OwnerTaskObservationUnavailableError:
                return py_trees.common.Status.RUNNING, task
            except Exception as exc:
                self._record_owner_error("owner_request_failed", exc)
                return py_trees.common.Status.FAILURE, task
            if observed.task_id != task.task_id:
                self._record_owner_failure(
                    "owner_task_identity_changed", "owner observation selected another task"
                )
                return py_trees.common.Status.FAILURE, task
            self._task = task = observed
            if task.metadata.get("interrupt_stop_terminal_pending") is not True:
                return None, task
        self._stop_terminal_waiting = True
        return py_trees.common.Status.RUNNING, task

    def _map_interrupt(self, task: OwnerTaskSnapshot) -> InterruptNodeResult | None:
        operation = task.metadata.get("interrupt_operation")
        if not isinstance(operation, Mapping):
            operation = {}
        chain = task.metadata.get("interrupt_stop_chain")
        if isinstance(chain, Mapping):
            chain = {key: tuple(value) for key, value in chain.items() if isinstance(value, (list, tuple))}
        else:
            chain = tuple(chain) if isinstance(chain, (list, tuple)) else ()
        terminal = task.metadata.get("interrupt_stop_terminal")
        contained = None
        if isinstance(terminal, Mapping):
            rows = [
                row
                for participant in operation.get("participants") or ()
                if isinstance(participant, Mapping)
                for row in participant.get("io_rule_outcomes") or ()
                if isinstance(row, Mapping)
            ]
            if len(rows) == 1 and isinstance(operation.get("operation_id"), str):
                contained = contained_stages_from_stop_terminal(
                    rows[0], terminal, operation_id=operation["operation_id"]
                )
        return map_interrupt_result(
            self._node_id,
            self._inputs or {},
            operation,
            stop_chain=chain,
            contained_stage_id=contained,
        )

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        if (
            self._phase is not _TaskPhase.OBSERVE
            or self._interrupt is not None
            or task.state is OwnerTaskState.ACTIVE
            or isinstance(task.metadata.get("preemption"), Mapping)
        ):
            return super()._settle(task)
        waiting, task = self._await_stop_terminal(task)
        if waiting is not None:
            return waiting
        result = self._map_interrupt(task)
        if result is None:
            # An ordinary Stop: the existing motion Stop handling applies.
            return super()._settle(task)
        self._interrupt = result
        self._outcome = result.node_outcome
        if self._audit_sink is not None:
            self._audit_sink(
                {
                    "kind": "program_node_interrupt_result",
                    "node_instance_id": self._node_instance_id,
                    "outcome": result.node_outcome,
                    "result": "success" if result.success else "failure",
                    "reason_code": result.reason_code,
                    "message": result.message,
                    "effect_uncertain": result.effect_uncertain,
                }
            )
        if result.success:
            if task.state is OwnerTaskState.SUCCEEDED:
                return super()._settle(task)
            # A trip contained within the rule's allowed stages: close the
            # stopped operation, then report success (contracts 69-71).
            self._phase = _TaskPhase.CLOSING
            return self._advance_close(task)
        failed = replace(
            task,
            reason_code=result.reason_code,
            message=result.message,
            **(
                {"state": OwnerTaskState.FAILED, "settlement_proven": True}
                if task.state is OwnerTaskState.SUCCEEDED
                else {}
            ),
        )
        if task.state is OwnerTaskState.SUCCEEDED:
            # A natural completion mapped to failure (on_no_signal): the
            # operation is acknowledged, then the node fails.
            failed = replace(
                failed,
                metadata={
                    key: value
                    for key, value in task.metadata.items()
                    if key not in {"authored_failure_policy", "authored_failure_policy_parameter"}
                },
            )
        # Normal failure handling (hold, close, the tree's fallback); no retry.
        return OwnerTaskAdapter._settle(self, failed)

    def _after_close(self, *, closed: bool) -> py_trees.common.Status:
        result = self._interrupt
        if result is None or not result.success:
            return super()._after_close(closed=closed)
        self._phase = _TaskPhase.DONE
        if closed:
            return py_trees.common.Status.SUCCESS
        self._record_owner_failure(
            "motion_interrupt_operation_retained",
            f"{result.message}; the stopped operation could not be closed (Reset releases it)",
        )
        return py_trees.common.Status.FAILURE


class GripperCommandAdapter(OwnerTaskAdapter):
    """Consume real delivery, then dwell on the run's existing Program clock."""

    def __init__(
        self,
        *,
        program_clock: ProgramClock,
        operator_stop_state: OperatorMotionStopState | None = None,
        operator_stop_hold_scope: str | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(operator_stop_state=operator_stop_state, **kwargs)
        self._dwell_time = AttemptTime(
            program_clock,
            operator_stop_state=(operator_stop_state if operator_stop_hold_scope is not None else None),
            operator_stop_hold_scope=operator_stop_hold_scope,
        )
        self._dwell_s: float | None = None
        self._timing_captured = False
        self._dwell_started = False
        self._draining_dwell = False

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._dwell_s = None
        self._timing_captured = False
        self._dwell_started = False

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        if not self._timing_captured:
            # Production captures these facts from the exact retained descriptor
            # before submit. Neither policy names nor missing metadata select IO.
            if task.metadata.get("gripper_actuation_kind") == "pneumatic":
                dwell = task.metadata.get("gripper_dwell_s")
                if (type(dwell) not in (int, float) or not math.isfinite(dwell)
                    or dwell < 0):
                    self._record_owner_failure("owner_result_invalid", "invalid captured Program dwell")
                    return py_trees.common.Status.FAILURE
                self._dwell_s = float(dwell)
            self._timing_captured = True
        if (task.state is OwnerTaskState.SUCCEEDED
            and self._dwell_s is not None and not self._dwell_started):
            # First observed actual Success, before ACK (which may still pend).
            # This local observation time is not a backend delivery timestamp.
            self._dwell_time.start(exclude_held=(self._inputs or {}).get("dwell_while_held", "pause") == "pause")
            self._dwell_started = True
        result = super()._settle(task)
        if (result is py_trees.common.Status.SUCCESS and self._dwell_s is not None
            and (self._draining_dwell or self._dwell_time.elapsed() < self._dwell_s)):
            return py_trees.common.Status.RUNNING
        return result

    def observe_while_operator_stop_held(self) -> None:
        # Delivery/ACK still drain while held. A due dwell is reported only
        # when normal scheduling is permitted again, like the existing Timer.
        self._draining_dwell = True
        try:
            super().observe_while_operator_stop_held()
        finally:
            self._draining_dwell = False

    def terminate(self, new_status: py_trees.common.Status) -> None:
        super().terminate(new_status)
        if new_status is py_trees.common.Status.INVALID:
            self._dwell_s = None
            self._dwell_started = False


# Contract 53: the host completes a write on correlated source acceptance, or
# on submission for a publish-only source (the strongest fact it supplies).
_OUTPUT_COMPLETION_EVIDENCE: Final = frozenset({"source_accepted", "submitted"})


class IoOutputCommandAdapter(OwnerTaskAdapter):
    """One typed boolean output write through the existing capability command.

    Completion is the host's correlated source acceptance (contract 53), never
    a physical effect or readback. A rejected/unsubmitted write is the
    Kernel's ordinary failure; unknown acceptance is the existing uncertain-
    effect handling (retained for intervention, never resent). Non-moving: no
    dwell and no Stop timing of its own.
    """

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._acceptance_unconfirmed = False

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._acceptance_unconfirmed = False

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        if self._acceptance_unconfirmed:
            return py_trees.common.Status.FAILURE
        if (
            task.state is OwnerTaskState.SUCCEEDED
            and self._phase is not _TaskPhase.DONE
            and task.metadata.get("output_evidence") not in _OUTPUT_COMPLETION_EVIDENCE
        ):
            # A completion without the source's correlated acceptance proves
            # nothing: the operation is retained (not acknowledged) for Reset.
            self._acceptance_unconfirmed = True
            self._record_owner_failure(
                "io_output_acceptance_unconfirmed",
                "output completed without correlated source acceptance "
                f"(evidence {task.metadata.get('output_evidence')!r})",
            )
            return py_trees.common.Status.FAILURE
        return super()._settle(task)


class OwnerInvocationAdapter(_OwnerAdapter):
    """Issue one bounded synchronous owner request per node attempt."""

    def __init__(self, *, port: OwnerInvocationPort,
                 operator_stop_state: OperatorMotionStopState | None = None, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._port = port
        self._invoked = False
        self._status = py_trees.common.Status.INVALID
        self._pending_observation = None
        self._operator_stop_state = operator_stop_state

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._invoked = False
        self._status = py_trees.common.Status.INVALID
        self._pending_observation = None

    def update(self) -> py_trees.common.Status:
        if self._wait_for_admission_release():
            return py_trees.common.Status.RUNNING
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        if self._invoked and self._pending_observation is None:
            return self._status
        try:
            if self._pending_observation is not None:
                result = self._pending_observation()
                if result is None:
                    return py_trees.common.Status.RUNNING
            else:
                gate = self._mock_simulation_ready()
                if gate is not None:
                    self._invoked = gate is py_trees.common.Status.FAILURE
                    self._status = gate
                    return gate
                self._invoked = True
                result = self._port.invoke(self._node_id, self._identity, self._owner_inputs)
        except OwnerInvocationHeldError as error:
            self._pending_observation = None
            self._invoked = False
            self._hold_before_admission(error)
            return py_trees.common.Status.RUNNING
        except OwnerInvocationPendingError as error:
            self._pending_observation = error.observe
            self.feedback_message = str(error)
            return py_trees.common.Status.RUNNING
        except Exception as exc:
            if self._pending_observation is not None:
                # Lookup/transport failure cannot settle a submitted effect.
                self.feedback_message = "Owner transaction outcome remains unconfirmed"
                return py_trees.common.Status.RUNNING
            self._record_owner_error("owner_request_failed", exc)
            self._status = py_trees.common.Status.FAILURE
            return self._status
        self._pending_observation = None
        if result.succeeded:
            self._status = (
                py_trees.common.Status.SUCCESS
                if self._write_output(
                    result.output,
                    owner_metadata=result.metadata,
                    adapter_metadata={
                        "program_run_id": self._identity.program_run_id,
                        "node_attempt_id": self._identity.node_attempt_id,
                    },
                )
                else py_trees.common.Status.FAILURE
            )
        else:
            self._record_owner_failure(result.reason_code, result.message, result.details)
            self._status = py_trees.common.Status.FAILURE
        return self._status

    @property
    def has_active_task(self) -> bool:
        return self._pending_observation is not None

    def observe_control_state(self) -> str:
        return "active" if self.has_active_task else "settled" if self._invoked else "inactive"

    def cached_control_state(self) -> str:
        return self.observe_control_state()

    def reconcile_full_stop_task(self, *, selected_resume: bool = False) -> str:
        # A selected Resume must not reopen admission while a previously sent
        # transaction is still in flight. Full tree Stop uses the permanent
        # run fence and the owner's child claim; it does not cancel this call.
        if not selected_resume:
            return "unaffected"
        self.observe_while_operator_stop_held()
        return self.observe_control_state()

    def observe_while_operator_stop_held(self) -> None:
        if self._pending_observation is not None:
            status = self.update()
            if status is not py_trees.common.Status.RUNNING:
                self.stop(status)


class GripperStatusWaitAdapter(_OwnerAdapter):
    """Read one command scope at at most 10 Hz using the existing tree ticks.

    Cancelling a read-only waiter has no physical command to cancel. It never
    follows a replacement command/source, renews evidence, or owns a worker.
    A Program Stop, Reset, cancel or a parent ending the wait reports
    `interrupted`. `timeout_s` counts the run's Program time, which stands
    still while the run is halted; with `timeout_while_held: pause` (the
    default) the time an operator Stop holds the wait does not count either,
    so a Stop never turns into a timeout failure when the wait resumes.
    """

    def __init__(
        self,
        *,
        port: OwnerInvocationPort,
        program_clock: ProgramClock,
        operator_stop_state: OperatorMotionStopState | None = None,
        operator_stop_hold_scope: str | None = None,
        **kwargs: Any,
    ):
        super().__init__(**kwargs)
        self._port = port
        self._clock = program_clock
        self._attempt_time = AttemptTime(
            program_clock,
            operator_stop_state=operator_stop_state,
            operator_stop_hold_scope=operator_stop_hold_scope,
        )
        self._next_read = 0.0
        self._scope = None

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._scope = None
        self._next_read = self._clock.now()
        self._attempt_time.start(
            exclude_held=self._inputs is not None
            and self._inputs.get("timeout_while_held") == "pause"
        )

    def _timed_out(self) -> bool:
        assert self._inputs is not None  # Checked before every deadline test.
        return self._attempt_time.elapsed() >= float(self._inputs["timeout_s"])

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._record_interruption(new_status)

    def update(self) -> py_trees.common.Status:
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        if self._timed_out():
            self._record_node_failure(
                "gripper_wait_timeout",
                "Expected gripper state was not observed before the deadline.",
            )
            return py_trees.common.Status.FAILURE
        if self._clock.now() < self._next_read:
            return py_trees.common.Status.RUNNING
        try:
            result = self._port.invoke(self._node_id, self._identity, self._owner_inputs)
            scope = result.metadata["command_scope"]
            if not isinstance(scope, tuple) or len(scope) != 7:
                raise ValueError("gripper query omitted its exact command/source identity")
        except Exception as exc:
            self._record_owner_error("gripper_wait_source_unavailable", exc)
            return py_trees.common.Status.FAILURE
        self._next_read = self._clock.now() + 0.1
        if self._scope is None:
            self._scope = scope
        if scope != self._scope:
            self._record_node_failure(
                "gripper_wait_stale", "The captured command or source was replaced."
            )
            return py_trees.common.Status.FAILURE
        if self._timed_out():
            self._record_node_failure(
                "gripper_wait_timeout", "Gripper read completed after the deadline."
            )
            return py_trees.common.Status.FAILURE
        if result.succeeded:
            return py_trees.common.Status.SUCCESS
        if (
            result.reason_code != "gripper_status_not_matched"
            or result.metadata.get("command_state") == "failed"
        ):
            self._record_owner_failure(result.reason_code, result.message, result.details)
            return py_trees.common.Status.FAILURE
        self.feedback_message = result.message or "Waiting for the captured gripper command."
        return py_trees.common.Status.RUNNING


def _io_reason(error: BaseException) -> str:
    """An owner refusal's stable `io_*` reason (spec 210 §5.2), if it has one."""

    details = getattr(error, "details", None)
    for candidate in (
        details.get("reason") if isinstance(details, Mapping) else None,
        getattr(error, "code", None),
    ):
        if isinstance(candidate, str) and candidate.startswith("io_"):
            return candidate
    return "owner_request_failed"


class IoInputWaitAdapter(_OwnerAdapter):
    """Wait for one boolean input level through one zero-claim owner watch.

    Each attempt registers its own watch and reads it once per tree tick,
    the Runner's normal task-observation cadence; the host latches a match
    on its own, so the cadence only affects notification latency. While the
    source is not fresh the wait stays RUNNING and visibly unavailable (D3).
    The optional `timeout_s` counts the attempt's Program time (it stands
    still during a Halt and, like gripper waits, while an operator Stop
    holds the node); expiry fails the node. The node's end, a Program Stop,
    Reset, cancel or a parent ending it releases the exact watch; a Halt or
    an operator Stop hold keeps it, and a match is reported only when the
    tree ticks again. A transport failure never releases it. A registration
    whose reply is unknown fails the attempt; the port then releases that
    exact attempt's scope without a token.
    """

    def __init__(
        self,
        *,
        port: OwnerTaskPort,
        program_clock: ProgramClock,
        owner_port_name: str = "io_watches",
        operator_stop_state: OperatorMotionStopState | None = None,
        operator_stop_hold_scope: str | None = None,
        audit_sink: Callable[[dict[str, Any]], None] | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self._port = port
        self._owner_port_name = owner_port_name
        self._attempt_time = AttemptTime(
            program_clock,
            operator_stop_state=operator_stop_state,
            operator_stop_hold_scope=operator_stop_hold_scope,
        )
        self._audit_sink = audit_sink
        self._task: OwnerTaskSnapshot | None = None
        self._source_unavailable = False

    def _begin_attempt(self, *, restart_series: bool) -> None:
        was_unavailable = self._source_unavailable
        self._release_watch()  # A new attempt always takes a new watch.
        super()._begin_attempt(restart_series=restart_series)
        self._task = None
        self._source_unavailable = False
        self._attempt_time.start(exclude_held=True)
        if was_unavailable:
            # The previous attempt last reported the source unavailable and the
            # new one starts without that state: close the transition here, or
            # the GUI keeps the unavailable state until the next read.
            self._record_source_transition(unavailable=False, quality=None)

    def _release_watch(self) -> None:
        task, identity, self._task = self._task, self._identity, None
        if task is None or identity is None:
            return
        try:
            if task.state is OwnerTaskState.SUCCEEDED:
                self._port.acknowledge(task.task_id, identity)
            else:
                self._port.cancel(task.task_id, identity)
        except Exception:
            # Unconfirmed: the port keeps the exact token and run teardown
            # releases it; the host also releases a retired run's watches.
            pass

    def _note_availability(self, quality: object) -> None:
        unavailable = quality != "fresh"
        self.feedback_message = (
            f"Waiting — input unavailable ({quality})" if unavailable
            else "Waiting for the input level."
        )
        if unavailable == self._source_unavailable:
            return
        self._source_unavailable = unavailable
        self._record_source_transition(unavailable=unavailable, quality=quality)

    def _record_source_transition(self, *, unavailable: bool, quality: object) -> None:
        if self._audit_sink is not None and self._identity is not None:
            self._audit_sink(
                {
                    "kind": (
                        "program_node_io_source_unavailable"
                        if unavailable
                        else "program_node_io_source_available"
                    ),
                    "node_instance_id": self._node_instance_id,
                    "node_attempt_id": self._identity.node_attempt_id,
                    "quality": quality if isinstance(quality, str) else None,
                }
            )

    def _timed_out(self) -> bool:
        assert self._inputs is not None  # Checked before every deadline test.
        timeout = self._inputs.get("timeout_s")
        return timeout is not None and self._attempt_time.elapsed() >= float(timeout)

    def update(self) -> py_trees.common.Status:
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        if self._timed_out():
            self._release_watch()
            self._record_node_failure(
                "io_wait_timeout",
                "The expected input level was not observed before timeout_s.",
            )
            return py_trees.common.Status.FAILURE
        try:
            if self._task is None:
                gate = self._mock_simulation_ready()
                if gate is not None:
                    return gate
                self._task = self._port.start(self._node_id, self._identity, self._owner_inputs)
            else:
                try:
                    observed = self._port.observe(self._task.task_id)
                except OwnerTaskObservationUnavailableError:
                    # Not teardown: the same watch is read again next tick.
                    self.feedback_message = "Waiting — input watch temporarily unreadable"
                    return py_trees.common.Status.RUNNING
                if observed.task_id != self._task.task_id:
                    self._record_owner_failure(
                        "owner_task_identity_changed", "owner observation selected another task"
                    )
                    self._release_watch()
                    return py_trees.common.Status.FAILURE
                self._task = observed
        except Exception as exc:
            self._record_owner_error(_io_reason(exc), exc)
            self._release_watch()
            return py_trees.common.Status.FAILURE
        if self._timed_out():
            # The synchronous register/read crossed the deadline: its result
            # (even a match) arrived after timeout_s and is not accepted.
            self._release_watch()
            self._record_node_failure(
                "io_wait_timeout",
                "The input read completed after timeout_s.",
            )
            return py_trees.common.Status.FAILURE
        task = self._task
        if task.state is OwnerTaskState.ACTIVE:
            self._note_availability(task.metadata.get("quality"))
            return py_trees.common.Status.RUNNING
        self._release_watch()
        if task.state is OwnerTaskState.SUCCEEDED:
            return py_trees.common.Status.SUCCESS
        self._record_owner_failure(task.reason_code, task.message, task.details)
        return py_trees.common.Status.FAILURE

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._record_interruption(new_status)
        self._release_watch()

    # A watch holds no claim: it is never owner work that Stop, Halt or a
    # selected Resume has to settle, and a hold does not observe it.
    @property
    def has_active_task(self) -> bool:
        return False

    def observe_control_state(self) -> str:
        return "inactive"

    def cached_control_state(self) -> str:
        return "inactive"

    def reconcile_full_stop_task(self, *, selected_resume: bool = False) -> str:
        return "unaffected"

    def observe_while_operator_stop_held(self) -> None:
        return None


__all__ = [
    "IoInputWaitAdapter",
    "OwnerInvocationAdapter",
    "OwnerTaskAdapter",
    "MotionTaskAdapter",
    "InterruptMotionTaskAdapter",
    "GripperCommandAdapter",
    "IoOutputCommandAdapter",
    "GripperStatusWaitAdapter",
]
