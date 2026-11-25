"""Thin v2 py_trees leaves over focused headless owner ports."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from dataclasses import dataclass, replace
from enum import Enum
import math
import time
from typing import Any, ClassVar, Final

import py_trees

from manyforge_behavior.kernel_client import (
    PLANNING_JOB_FINAL_STATES,
    KernelOperationAdmissionRejectedError,
    KernelOperationObservationUnavailableError,
)
from manyforge_behavior.failure_details import (
    NodeFailureOrigin,
    exception_failure_details,
)
from manyforge_behavior.orchestration_ports import (
    PROGRAM_MOTION_STOP_RECOVERY_CLASS,
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
    OwnerTaskReadRequiredError,
    OwnerTaskSettlementPendingError,
    OwnerTaskSnapshot,
    OwnerTaskState,
    PoseSourceLatchError,
    ProgramInterventionPort,
    is_ordinary_operator_stop,
    describe_owner_failure,
)
from manyforge_behavior.request_keys import Phase, program_key
from manyforge_behavior.interrupt_outcome import (
    InterruptNodeResult,
    interrupt_stop_pending,
    map_interrupt_result,
)
from manyforge_behavior.nodes.interruption import NODE_INTERRUPTED_REASON, InterruptedOutcome
from manyforge_behavior.nodes.v2_pure import PublishedWriteHold
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopState
from manyforge_behavior.program_clock import AttemptTime, ProgramClock
from manyforge_behavior.run_sender import (
    RunRequest,
    RunRequestState,
    RunSender,
)
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


class _OwnerAdapter(PublishedWriteHold, InterruptedOutcome, py_trees.behaviour.Behaviour):
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
        derived_active_tcp_key: str | None = None,
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
        self._derived_active_tcp_key = derived_active_tcp_key
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
        # The index orders this node's starts within the Run:
        # +1 per attempt on its first send, kept across re-entries.
        self._mock_simulation = mock_simulation
        self._mock_start_index = 0
        self._mock_request: Mapping[str, Any] | None = None
        # The run's ordered sender (OD-213, OD-380 D2), set by the leaves
        # whose owner requests leave the tick; None: made inside the tick.
        self._sender: RunSender | None = None
        # This attempt's Mock START request being built, and its START, on
        # the run's sender.
        self._mock_build: RunRequest | None = None
        self._mock_start: RunRequest | None = None
        self._mock_request_attempt: str | None = None
        self._mock_acknowledged_attempt: str | None = None
        self._mock_ended_attempt: str | None = None
        # The latest send of the Run's pending Mock ENDs this node asked for
        # on the run's sender (at most one of them queued).
        self._mock_end_send: RunRequest | None = None
        # Operator Stop scope reads on the run's sender (OD-380 D2): the one
        # a hold before admission asks for (its answer is published to the
        # run's Stop state), and the one an operator-Stopped task waits for.
        self._admission_scope_read: RunRequest | None = None
        self._stop_scope_read: RunRequest | None = None
        # A failed `_stop_scope_read` raises (the operator-Stop settle's
        # read, as base's inline one did) or not (a stopped unstarted task's).
        self._stop_scope_read_raises = True

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
        self._forget_written()
        self._stop_scope_read = None  # Its answer belongs to that attempt.
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
                **(
                    {"derived_active_tcp_key": self._derived_active_tcp_key}
                    if self._derived_active_tcp_key is not None else {}
                ),
            )
            self._inputs = snapshot.parameters
            self._owner_inputs = OwnerNodeInputs(
                parameters=self._inputs,
                blackboard=snapshot.blackboard,
            )
        except (KeyError, ProgramValueError, TypeError, ValueError) as exc:
            # The inputs fail before any owner request, whatever outcomes the
            # catalog entry declares.
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

    @property
    def attempt_inputs(self) -> Mapping[str, Any] | None:
        """This attempt's resolved parameters; None while they are unresolved."""

        return self._inputs

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
            receipt = self._input_resolver.value_store.write(
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
        if receipt is not None:
            self._written = receipt  # The result waits for the host's answer.
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
        """This attempt's literal Mock simulation is
        acknowledged before the node's action or read.

        None: acknowledged, or nothing authored; act now. RUNNING: the
        START is on the run's sender, or its reply is unknown and the same
        request (same index and attempt) is sent again on the next tick,
        never a new attempt. FAILURE: the host refused it. Only ticks submit,
        so a Halt, a Stop or the parent ending the node sends no more (a
        Halt withdraws a START not sent yet; the node submits it again).
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
                if self._sender is None:
                    request = self._mock_simulation.request(identity, index, rows)
                else:
                    # The request carries the run's claim generation, a
                    # Kernel scope read: built on the run's sender too.
                    built = self._mock_build
                    if built is None or built.withdrawn:
                        port = self._mock_simulation
                        self._mock_build = self._sender.submit(
                            lambda: port.request(identity, index, rows)
                        )
                        self.feedback_message = "Waiting for Mock simulation acknowledgement"
                        return py_trees.common.Status.RUNNING
                    if not built.answered:
                        return py_trees.common.Status.RUNNING
                    self._mock_build = None
                    request = built.result()
            except Exception as exc:
                self._record_owner_error("mock_simulation_refused", exc)
                return py_trees.common.Status.FAILURE
            self._mock_start_index = index
            self._mock_request = request
            self._mock_request_attempt = attempt_id
        assert self._mock_request is not None  # Built with its attempt.
        try:
            if self._sender is None:
                ack = self._mock_simulation.start(self._mock_request)
            else:
                sent = self._mock_start
                if sent is None or sent.withdrawn:
                    return self._send_mock_start()
                if not sent.answered:
                    return py_trees.common.Status.RUNNING
                self._mock_start = None
                ack = sent.result()
        except MockSimulationRefusedError as error:
            self._record_owner_failure(
                "mock_simulation_refused",
                describe_owner_failure(f"{error.code}: {error}", error.details),
                error.details,
            )
            return py_trees.common.Status.FAILURE
        except Exception:
            if self._sender is not None:
                # The reply is unknown: the same START is sent again.
                return self._send_mock_start()
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

    def _send_mock_start(self) -> py_trees.common.Status:
        assert self._sender is not None and self._mock_request is not None
        port, start = self._mock_simulation, self._mock_request
        assert port is not None
        self._mock_start = self._sender.submit(lambda: port.start(start))
        self.feedback_message = "Waiting for Mock simulation acknowledgement"
        return py_trees.common.Status.RUNNING

    def _mock_simulation_end(self) -> None:
        """The attempt that sent its Mock simulation start
        ended, so its still-pending change is cancelled. Once per attempt;
        a Halt is no end (the tree is not stopped)."""

        if self._mock_build is not None:
            self._mock_build.withdraw()  # Builds only: nothing started.
            self._mock_build = None
        if self._mock_start is not None:
            # A START not sent is never sent. A sent one completes; an END
            # that reaches the host first makes it inert there
            # (`mock_simulation_ended`).
            self._mock_start.withdraw()
            self._mock_start = None
        attempt_id = self._mock_request_attempt
        if (
            self._mock_request is None
            or self._mock_simulation is None
            or attempt_id is None
            or self._mock_ended_attempt == attempt_id
        ):
            return
        self._mock_ended_attempt = attempt_id
        port, start = self._mock_simulation, self._mock_request
        try:
            # The END is pending in the port (memory only); the run's sender
            # sends it, also after a Halt or the run's end (never
            # withdrawn), after this attempt's START. An unknown reply stays
            # pending: the run resends it unchanged on its sender and
            # reports it if it never gets an ACK.
            port.end(start)
            send = self._mock_end_send
            if self._sender is None:
                port.resend_unknown_ends(start["program_run_id"])
            elif send is None or send.state is not RunRequestState.QUEUED:
                self._mock_end_send = self._sender.submit(
                    lambda: port.resend_unknown_ends(start["program_run_id"]),
                    withdrawable=False,
                )
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
            or scope.hold_revision <= self._held_before_admission
            or self._node_instance_id in scope.held_nodes):
            return True
        # Proven-unstarted work, fresh attempt after owner release.
        self._begin_attempt(restart_series=False)
        return False

    def _hold_before_admission(self, error: OwnerInvocationHeldError) -> None:
        self._held_before_admission = error.hold_revision
        self.feedback_message = str(error)
        state = self._operator_stop_state
        if state is None:
            return
        if self._sender is None:
            try:
                state.refresh()
            except Exception:
                pass  # Retain the exact fence until an authoritative newer release.
            return
        # The scope read leaves the tick; its answer is published to the
        # run's Stop state for a later tick. Until then (and after a failed
        # read) the node keeps the exact fence: it waits.
        read = self._admission_scope_read
        if read is None or read.answered:
            self._admission_scope_read = self._sender.submit(
                state.refresh, withdrawable=False
            )

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
    """The Kernel's own close permission on the exact
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
    intervention/containment handling, never waited on."""

    if task.metadata.get("operation_state") == "intervention_required":
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
    retirement already completed. A failed read is not this proof."""

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


# A run's failure message bound (`FailureRecord`).
_MAX_FAILURE_MESSAGE_BYTES: Final = 4_096


def _terminal_cleanup_pending(task: OwnerTaskSnapshot) -> bool:
    """The ended (failed or stopped) execution's terminal cleanup still runs:
    not yet decided."""

    assessment = task.metadata.get("recovery_assessment")
    return (
        isinstance(assessment, Mapping)
        and assessment.get("status") == "waiting"
        and assessment.get("reason_code") == "terminal_cleanup_pending"
    )


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

    With the run's sender the start leaves the tick (Set TCP; OD-380 D2: the
    motion and gripper starts, whose answer is a Kernel operation): the node
    is RUNNING until its answer, which wakes the run's tick (S2); the answer
    registers the operation's watch, so its pushed changes wake the tick
    too. An operation start the tree stops (INVALID) after it was sent is
    followed by the run's tick (`drain_invalidated`) until its answer: an
    accepted operation is then cancelled and drained exactly as if it had
    been started inside the tick before the stop.

    With the run's sender the settling leaves the tick too (OD-380 D2): the
    tick observes from memory (the pushed table); a read the observation
    needs (a recovery assessment), the acknowledgement and the close go on
    the sender, one at a time, and the node stays RUNNING until the answer.
    A read is sent again on a later tick, never on the tick that took the
    previous answer: the base cadence of one read per tick, no time limit.
    Base made the start and these steps inside the tick that read their
    answer, so a Stop (INVALID) that finds a sent start or a settle step on
    the sender lets the attempt take it first (`_continue_stopped`): it ends
    with its real result, or the Stop then applies as to a running attempt.
    The node's halt (`terminate`) still stops its operation at once (the
    control lane), and the Program's Stop nodes have no sender: they send
    their Stop and read its status inside the tick, on the host's Stop
    control socket, never behind the run's other requests (OD-119).
    """

    # The start's answer is a Kernel operation (motion, gripper), not an
    # atomic step: a sent start the tree stops is followed to its answer.
    _START_OPENS_OPERATION: ClassVar[bool] = False

    def __init__(
        self,
        *,
        port: OwnerTaskPort,
        owner_port_name: str = "unspecified",
        intervention_port: ProgramInterventionPort | None = None,
        require_initial_running: bool = False,
        operator_stop_state: OperatorMotionStopState | None = None,
        run_sender: RunSender | None = None,
        program_stop_starts: RunSender | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        # The run's sender for this node's start (OD-213: Set TCP): the start
        # leaves the tick and the node is RUNNING until its answer. None: the
        # start is made inside the tick.
        self._sender = run_sender
        # A Program Stop node's run sender (its Stop is never sent on it):
        # the Stop withdraws the run's queued motion and gripper starts it
        # covers and follows one in flight (`RunSender.stop_starts`).
        self._program_stop_starts = program_stop_starts
        # This attempt's request on the run's sender (its start; the
        # pneumatic gripper's valve write), and a sent one the tree stopped.
        self._request: RunRequest | None = None
        self._abandoned: RunRequest | None = None
        # The stopped attempt's acknowledgement on the run's sender and the
        # result it ends with once that is answered (Forced Program End).
        self._abandoned_ack: tuple[RunRequest, py_trees.common.Status] | None = None
        # `_request` is an operation start (`_START_OPENS_OPERATION`).
        self._request_opens_operation = False
        # A Program Stop withdrew this attempt's start before it was sent
        # (`_start_stopped_before_sent` decides how the attempt ends).
        self._start_stopped = False
        # A stopped attempt's sent operation start, with its identity: the
        # run's tick takes its answer and stops what it started.
        self._follow_start: tuple[RunRequest, NodeAttemptIdentity | None] | None = None
        # The settling's owner request on the run's sender (OD-380 D2): its
        # step ("observe", "acknowledge" or "close"), its task, the request,
        # and whether it is withdrawable (a settle step of a terminal task,
        # made inside the tick that saw it in base, is not).
        self._settle_request: tuple[str, int | str, RunRequest, bool] | None = None
        # A Stop (INVALID) found this attempt's sent start or a settle step
        # on the run's sender: the attempt goes on off the tree until it ends
        # with its real result or is RUNNING with nothing sent; then the Stop
        # applies (`_continue_stopped`).
        self._stopped_settling = False
        self._applying_stop = False
        self._port = port
        self._owner_port_name = owner_port_name
        self._intervention_port = intervention_port
        self._require_initial_running = require_initial_running
        self._operator_stop_state = operator_stop_state
        self._task: OwnerTaskSnapshot | None = None
        self._phase = _TaskPhase.SUBMIT
        # An abandoned attempt's task still to close, with its identity.
        self._drain: tuple[OwnerTaskSnapshot, NodeAttemptIdentity] | None = None
        self._holds_failure = False
        # The failed operation's own facts when its failure reached the tree.
        self._operation_failure: Mapping[str, Any] | None = None
        # Refused with `simulation_not_playing`; the run owner's
        # operator Stop for it is still to be sent (`simulation_stop_sent`).
        self.simulation_stop_pending = False
        # The attempt that hold holds, until the next attempt ends it.
        self._simulation_hold_attempt: str | None = None

    def _hold_for_simulation(self, error: KernelOperationAdmissionRejectedError) -> None:
        """The command was refused because its simulation isn't
        playing. Nothing started; the node holds before admission exactly as
        under the operator's Stop (shown "waiting for Run") and the run owner
        sends that Stop on its groups. The operator's Run releases it and the
        next tick submits a fresh attempt (refused and held again while the
        simulation still isn't playing)."""

        state = self._operator_stop_state
        scope = None if state is None else state.snapshot()
        self._held_before_admission = 0 if scope is None else scope.hold_revision
        self.feedback_message = str(error)
        self.simulation_stop_pending = True
        attempt = None if self._identity is None else self._identity.node_attempt_id
        self._simulation_hold_attempt = attempt
        if self._audit_sink is not None:
            self._audit_sink(
                {
                    "kind": "program_node_simulation_not_playing",
                    "node_instance_id": self._node_instance_id,
                    "node_attempt_id": attempt,
                    "code": error.code,
                    "message": str(error),
                }
            )

    def simulation_stop_sent(self) -> None:
        """The run owner's operator Stop for this hold was accepted."""

        self.simulation_stop_pending = False

    def hold_failures_for_intervention(self) -> None:
        """This node's own recovery policy holds its failures: the failed
        operation stays live for the operator instead of closing."""

        self._holds_failure = True

    def _begin_attempt(self, *, restart_series: bool) -> None:
        if self._stopped_settling:
            # Entered again before the stopped attempt's request was taken:
            # the Stop applies now. Its sent start is followed by the drain;
            # its terminal task is drained (cancelled first while active).
            self._stopped_settling = False
            request, self._request = self._request, None
            if request is not None:
                self._follow_start = (request, self._identity)
            else:
                self._stop_task()
        held = self._simulation_hold_attempt
        if held is not None:
            # The simulation hold ends with its attempt (Run, re-entry):
            # the GUI's "Simulation not playing" line goes with it.
            self._simulation_hold_attempt = None
            if self._audit_sink is not None:
                self._audit_sink(
                    {
                        "kind": "program_node_simulation_hold_ended",
                        "node_instance_id": self._node_instance_id,
                        "node_attempt_id": held,
                    }
                )
        super()._begin_attempt(restart_series=restart_series)
        if self._request is not None:
            self._request.withdraw()
        self._request = None
        self._start_stopped = False
        self._abandoned = None
        self._abandoned_ack = None
        self._forget_settling()
        self._task = None
        self._phase = _TaskPhase.SUBMIT
        self._operation_failure = None
        self.simulation_stop_pending = False

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
            task is None or type(task.metadata.get("operation_id")) is int
        )

    def _close_request_key(self, identity: NodeAttemptIdentity) -> bytes:
        return program_key(identity.program_run_id, identity.node_attempt_id, Phase.PROGRAM_CLOSE)

    def _forget_settling(self) -> None:
        """The attempt ended: its settling request is withdrawn (a sent one
        completes, unread), unless the drain of its operation takes it."""

        settling = self._settle_request
        if (
            settling is not None
            and not self._stopped_settling  # The stopped attempt still takes it.
            and (self._drain is None or settling[1] != self._drain[0].task_id)
        ):
            settling[2].withdraw()
            self._settle_request = None

    def _owner_call(
        self,
        step: str,
        task_id: int | str,
        call: Callable[[], Any],
        *,
        withdrawable: bool = True,
    ) -> RunRequest | _InlineRequest | None:
        """One owner call of the settling (OD-380 D2) on the run's sender:
        None while it is not answered (sent now if it is not on the sender
        yet), then the answered request, once (``result()`` gives its answer
        or raises what it raised). Another step's request still on the
        sender is withdrawn (a sent one completes, unread). Without the
        run's sender the call is made at once.

        A settle step of a terminal task (its read, acknowledgement, close,
        grasp read, watch release) is not ``withdrawable``: base made it
        inside the tick that saw the task end, so a Halt or Stop never
        drops it (a Stop lets the attempt take it, `_continue_stopped`)."""

        if self._sender is None:
            return _InlineRequest(call)
        settling = self._settle_request
        if settling is not None:
            pending_step, pending_task, request, _withdrawable = settling
            if pending_step == step and pending_task == task_id and not request.withdrawn:
                if not request.answered:
                    return None
                self._settle_request = None
                return request
            request.withdraw()
        request = self._sender.submit(call, withdrawable=withdrawable)
        self._settle_request = (step, task_id, request, withdrawable)
        if not request.answered:
            return None
        # Answered at once (a sender that runs its requests in place).
        self._settle_request = None
        return request

    def _observed(
        self, task_id: int | str, *, settling: bool = False
    ) -> OwnerTaskSnapshot | None:
        """The task's observation: memory only in the tick (the pushed
        table). A read it needs goes on the run's sender (`_owner_call`):
        None until its answer, read on a later tick. What the observation
        raised is raised here. ``settling``: the read of a terminal task's
        settlement (not withdrawable)."""

        pending = self._settle_request
        if (
            pending is not None
            and pending[0] == "observe"
            and pending[1] == task_id
            and not pending[2].withdrawn
        ):
            # The read on the sender: its answer (None while unanswered).
            answered = self._owner_call("observe", task_id, lambda: None)
        else:
            try:
                return self._port.observe(task_id)
            except OwnerTaskReadRequiredError as needed:
                answered = self._owner_call(
                    "observe", task_id, needed.read, withdrawable=not settling
                )
        return None if answered is None else answered.result()

    def _sent_acknowledge(
        self, task_id: int | str, identity: NodeAttemptIdentity
    ) -> RunRequest | _InlineRequest | None:
        port = self._port
        return self._owner_call(
            "acknowledge",
            task_id,
            lambda: port.acknowledge(task_id, identity),
            withdrawable=False,
        )

    def _sent_close(
        self, task_id: int | str, identity: NodeAttemptIdentity
    ) -> RunRequest | _InlineRequest | None:
        port, request_key = self._port, self._close_request_key(identity)
        return self._owner_call(
            "close",
            task_id,
            lambda: port.close(task_id, identity, request_key=request_key),  # type: ignore[attr-defined]
            withdrawable=False,
        )

    def _record_pose_source_latch_failure(self, error: PoseSourceLatchError) -> None:
        self._phase = _TaskPhase.DONE
        self._record_owner_failure(
            error.reason_code,
            describe_owner_failure(str(error), error.details),
            error.details,
        )

    def _await_settled_assessment(
        self, task: OwnerTaskSnapshot
    ) -> tuple[py_trees.common.Status | None, OwnerTaskSnapshot]:
        """A failure can arrive before its assessment is decided. Reread the
        exact current operation, read-only, once per tick (on the run's
        sender, `_observed`): (RUNNING, task) while it is not yet decidable
        or the read is unanswered, failed and stopped alike (a stopped
        operation also after a failed read, `_failed_read`), (FAILURE, task)
        for any other failed read, and (None, task) once the read decides its
        closure (`_close_decidable`). No time limit (OD-214): the Kernel
        republishes when the execution's terminal cleanup ends."""

        if _close_decidable(task):
            return None, task
        try:
            observed = self._observed(task.task_id, settling=True)
        except OwnerTaskObservationUnavailableError:
            if task.state is OwnerTaskState.INTERRUPTED:
                return self._failed_read(task), task
            return py_trees.common.Status.RUNNING, task
        except Exception as exc:
            self._record_owner_error("owner_request_failed", exc)
            return self._failed_read(task), task
        if observed is None:
            return py_trees.common.Status.RUNNING, task
        if observed.task_id != task.task_id:
            self._record_owner_failure(
                "owner_task_identity_changed",
                "owner observation selected another task",
            )
            return self._failed_read(task), task
        self._task = observed
        if _close_decidable(observed):
            # A decidable fact wins over any read problem beside it.
            return None, observed
        read_failure = observed.metadata.get("recovery_assessment_failure")
        if isinstance(read_failure, Mapping):
            # A malformed or unexpected read is not temporary absence. It
            # never replaces the operation's own failure: that failure is the
            # node's reason, the read problem is named in its message.
            self._record_operation_failure_with_read_problem(observed, read_failure)
            return self._failed_read(observed), observed
        return py_trees.common.Status.RUNNING, observed

    def _record_operation_failure_with_read_problem(
        self, task: OwnerTaskSnapshot, read_failure: Mapping[str, Any]
    ) -> None:
        read_reason = str(read_failure.get("reason_code") or "owner_recovery_assessment_invalid")
        read_message = str(read_failure.get("message") or "recovery assessment is invalid")
        problem = f"recovery assessment read failed: {read_reason[:256]}: {read_message[:256]}"
        if not task.reason_code:
            # Defensive: a failure always carries a reason.
            self._record_owner_failure(read_reason, read_message)
            return
        # Details stay the task's bounded details (registered flat keys only);
        # the read problem travels in the bounded human message.
        message = f"{task.message} ({problem})" if task.message else problem
        if len(message.encode("utf-8")) > _MAX_FAILURE_MESSAGE_BYTES:
            message = task.message
        self._record_owner_failure(task.reason_code, message, task.details)
        self._note_operation_failure(task)

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
            if self._holds_failure and not _close_eligible(task) and _retired(task):
                # Retirement already completed: nothing is left to hold, so
                # the hold has no owner task and its offers follow the
                # node's policy (OD-263), as `_advance_close` drops it.
                self._task = None
            self._phase = _TaskPhase.DONE
            return py_trees.common.Status.FAILURE
        self._phase = _TaskPhase.CLOSING
        return self._advance_close(task)

    def _advance_close(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        """Close the failed operation, then continue (`_after_close`).

        The exact current close permission decides first. Without it, an
        operation already retired (resident false) leaves nothing to close;
        otherwise (no permission, uncertain effects, an unresolved
        intervention, a refused or impossible close) the operation stays retained
        (Reset releases it; its alarm stands) and the failure is returned.
        An unknown close outcome keeps the node RUNNING; the same request
        replays. The close goes on the run's sender (OD-380 D2): RUNNING
        until its answer.
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
        answered = self._sent_close(task.task_id, self._identity)
        if answered is None:
            return py_trees.common.Status.RUNNING
        try:
            closed = answered.result()
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

    def _failed_read(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        """A failed read of a stopped operation leaves it undecided: never
        advance the tree past it (a Fallback could move that robot). It waits
        (RUNNING; the next tick rereads) until the component's activity leaves
        `unknown` and the Stop's normal result is readable. No time limit
        (OD-214): a lost event stream makes reads fail until it is back; a
        host exit ends the application; a Reset ends the run. Any other
        failed read fails as before."""

        if task.state is not OwnerTaskState.INTERRUPTED:
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.RUNNING

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        if self._phase is _TaskPhase.CLOSING:
            return self._advance_close(task)
        holding = self._phase is _TaskPhase.HOLDING
        assessment = task.metadata.get("recovery_assessment")
        operator_stop = (
            task.state is OwnerTaskState.INTERRUPTED
            and isinstance(assessment, Mapping)
            and assessment.get("recovery_class") == PROGRAM_MOTION_STOP_RECOVERY_CLASS
        )
        self._held_after_operator_stop = is_ordinary_operator_stop(task)
        # This pass read the operator-Stop scope (`_hold_stopped_unstarted`
        # then decides on it; base read it twice in the same tick).
        scope_read = False
        if operator_stop and self._operator_stop_state is not None:
            if (
                type(task.metadata.get("stop_event_id")) is not int
                or task.metadata["stop_event_id"] < 1
            ):
                self._record_owner_failure(
                    "owner_stop_identity_missing",
                    "ordinary operator Stop omitted its Stop event",
                )
                return py_trees.common.Status.FAILURE
            if not holding:
                if self._sender is None:
                    self._operator_stop_state.refresh()
                else:
                    # The scope read leaves the tick (OD-380 D2): RUNNING
                    # until its answer, then on as after an inline read (a
                    # failed read raises its error here, as base's did; one
                    # `_hold_stopped_unstarted` sent is never raised).
                    read = self._stop_scope_read
                    if read is None:
                        read = self._stop_scope_read = self._sender.submit(
                            self._operator_stop_state.refresh, withdrawable=False
                        )
                        self._stop_scope_read_raises = True
                    if not read.answered:
                        return py_trees.common.Status.RUNNING
                    self._stop_scope_read = None
                    if self._stop_scope_read_raises:
                        read.result()
                    scope_read = True
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
                # On the run's sender (OD-380 D2): RUNNING until its answer.
                answered = self._sent_acknowledge(task.task_id, self._identity)
                if answered is None:
                    return py_trees.common.Status.RUNNING
                try:
                    answered.result()
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
                if _terminal_cleanup_pending(task):
                    # Hold on the decided assessment (its Resume, Retry,
                    # Continue and Reset offers), not on the cleanup wait.
                    pending, task = self._await_settled_assessment(task)
                    if pending is not None:
                        return pending
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
        if self._hold_stopped_unstarted(task, scope_read=scope_read):
            return py_trees.common.Status.RUNNING
        return self._fail_to_tree(task)

    def _hold_stopped_unstarted(self, task: OwnerTaskSnapshot, *, scope_read: bool = False) -> bool:
        """A Stop that holds this node ended its operation and the Kernel
        already retired it (a take-over newcomer). With no effect nothing
        started (OD-264); with its effect settled the Kernel kept its rest
        (OD-269). Either way the node holds exactly as a Stop-fence refusal at
        admission does, and Resume's next attempt plans the rest (or a fresh
        attempt when nothing started). An uncertain effect still fails.

        The fresh scope it decides on is read on the run's sender (OD-380
        D2; not withdrawable, one at a time): True (RUNNING) until its
        answer, used on a later tick; a failed read leaves the last
        published scope to decide. ``scope_read``: this pass of `_settle`
        has just read it."""

        state = self._operator_stop_state
        fact = task.metadata.get("operation_failure")
        facts = fact.get("terminal_facts") if isinstance(fact, Mapping) else None
        if (
            state is None
            or task.state is not OwnerTaskState.INTERRUPTED
            or not _retired(task)
            or type(task.metadata.get("stop_event_id")) is not int
            or task.metadata["stop_event_id"] < 1
            or not isinstance(facts, (list, tuple))
            or not facts
            or any(
                not isinstance(item, Mapping)
                or item.get("effect") not in ("no_effect", "effect_settled")
                for item in facts
            )
        ):
            return False
        held = self._hold_if_stop_holds(task.message, scope_read=scope_read)
        return held is not False

    def _hold_if_stop_holds(self, message: str, *, scope_read: bool = False) -> bool | None:
        """On a fresh operator-Stop scope (`_hold_stopped_unstarted`): True
        when a Stop holds this node, which then holds as a Stop-fence refusal
        at admission does; False when none does; None while the scope read
        is on the run's sender."""

        state = self._operator_stop_state
        assert state is not None
        if self._sender is None:
            try:
                state.refresh()
            except Exception:
                pass  # The last published scope decides, as in `_hold_before_admission`.
        elif not scope_read:
            read = self._stop_scope_read
            if read is None:
                self._stop_scope_read = self._sender.submit(state.refresh, withdrawable=False)
                self._stop_scope_read_raises = False
                return None
            if not read.answered:
                return None
            self._stop_scope_read = None  # A failed read: the published scope decides.
        scope = state.snapshot()
        if self._node_instance_id not in scope.held_nodes:
            return False
        self._task = None
        self._phase = _TaskPhase.SUBMIT
        self._held_before_admission = scope.hold_revision
        self.feedback_message = message
        return True

    def _on_priority_refusal(
        self, error: OwnerInvocationRefusedByPriorityError
    ) -> py_trees.common.Status:
        if self._node_id == "motion.set_tcp" and isinstance(
            error.__cause__, KernelOperationAdmissionRejectedError
        ):
            self._record_owner_error(error.__cause__.code, error.__cause__)
            return py_trees.common.Status.FAILURE
        self._record_owner_error("owner_request_failed", error)
        return py_trees.common.Status.FAILURE

    def _finish(self, status: py_trees.common.Status) -> py_trees.common.Status:
        """Hook on every status this node returns (motion writes its outcome)."""

        return status

    def update(self) -> py_trees.common.Status:
        held = self._held_write_result()
        if held is not None:
            return held
        # A written output or outcome returns the result after the host's
        # answer to it (OD-213 part 3).
        return self._after_write(self._finish(self._update()))

    def _start_task(self) -> OwnerTaskSnapshot:
        assert self._identity is not None and self._owner_inputs is not None
        return self._port.start(self._node_id, self._identity, self._owner_inputs)

    def _sent_start(self) -> OwnerTaskSnapshot | None:
        """The start, made inside the tick or (with the run's sender) sent
        and read on a later tick: None while it is not answered. What the
        start raised is raised here, on the tick that reads it."""

        if self._sender is None:
            starts = self._program_stop_starts
            if starts is not None and starts.stop_starts(self._program_stop_target()):
                # A motion or gripper start's admission is in flight: this
                # Program Stop is sent once it answered (base sent it after
                # that start inside the same tick); queued ones are withdrawn.
                return None
            return self._start_task()
        request = self._request
        if request is None or request.withdrawn:
            self._request = self._sender.submit(
                self._watched(self._start_call()), start_target=self._start_target()
            )
            self._request_opens_operation = self._START_OPENS_OPERATION
            return None
        if not request.answered:
            return None
        self._request = None
        return request.result()

    def _start_target(self) -> Any:
        """A motion or gripper start: the target whose Kernel operation it
        opens, which a Program Stop on it withdraws while queued; else None."""

        if not self._START_OPENS_OPERATION:
            return None
        assert self._owner_inputs is not None
        target = self._owner_inputs.parameters.get("target")
        return target if isinstance(target, Mapping) else self._node_instance_id

    def _program_stop_target(self) -> Any:
        """The target this Program Stop covers (`kernel.stop_operation`), or
        None for the cell (`kernel.cell_stop`)."""

        if self._node_id != "kernel.stop_operation":
            return None
        assert self._owner_inputs is not None
        return self._owner_inputs.parameters.get("target")

    def _start_stopped_before_sent(self) -> py_trees.common.Status | None:
        """A Program Stop withdrew this attempt's motion or gripper start
        before it was sent (`RunSender.stop_starts`); nothing reached the
        host. It ends as base's start that such a Stop cancelled before
        dispatch (`operation_cancelled_pre_dispatch`, nothing started and
        nothing left resident): it holds when a Stop holds the node
        (`_hold_stopped_unstarted`), else its failure returns to the tree
        with nothing to close (`_fail_to_tree` of a retired operation).
        None when its start was not withdrawn by a Program Stop."""

        request = self._request
        if request is not None and request.withdrawn_by_program_stop:
            self._request = None
            self._start_stopped = True
        if not self._start_stopped:
            return None
        message = "native operation was interrupted"
        if self._operator_stop_state is not None:
            held = self._hold_if_stop_holds(message)
            if held is None:
                return py_trees.common.Status.RUNNING  # Its scope read is on the sender.
            if held:
                self._start_stopped = False
                return py_trees.common.Status.RUNNING
        self._start_stopped = False
        self._record_owner_failure("operation_cancelled_pre_dispatch", message)
        self._operation_failure = None
        self._task = None
        self._phase = _TaskPhase.DONE
        return py_trees.common.Status.FAILURE

    def _start_call(self) -> Callable[[], Any]:
        """The start as one call on the run's sender (its values taken now)."""

        assert self._identity is not None and self._owner_inputs is not None
        port, node_id = self._port, self._node_id
        identity, inputs = self._identity, self._owner_inputs
        return lambda: port.start(node_id, identity, inputs)

    def _watched(self, call: Callable[[], Any]) -> Callable[[], Any]:
        """On the sender's thread, before its answer wakes the tick: the
        accepted operation's pushed changes wake the run (S2)."""

        sender = self._sender
        assert sender is not None

        def start() -> Any:
            answer = call()
            if isinstance(answer, OwnerTaskSnapshot):
                operation_id = answer.metadata.get("operation_id")
                if type(operation_id) is int:
                    sender.watch_operation(operation_id)
            return answer

        return start

    @staticmethod
    def _started_task(request: RunRequest) -> OwnerTaskSnapshot | None:
        """What a stopped attempt's answered start started: None when it
        was not admitted (held, refused, failed) or started nothing."""

        try:
            answer = request.result()
        except Exception:
            return None
        return answer if isinstance(answer, OwnerTaskSnapshot) else None

    def _take_follow_start(self) -> bool:
        """The stopped attempt's sent start: False while unanswered; once
        answered, what it started is stopped as `terminate` stops a started
        attempt (cancelled while active, then drained). True then."""

        assert self._follow_start is not None
        request, identity = self._follow_start
        if not request.answered:
            return False
        self._follow_start = None
        task = self._started_task(request)
        if task is None or identity is None:
            return True
        if task.state is OwnerTaskState.ACTIVE:
            try:
                self._port.cancel(task.task_id, identity)
            except Exception as exc:
                # Not cancelled: the stop is not confirmed (as in `terminate`),
                # unless a new attempt began meanwhile (it is not this one's).
                if identity == self._identity:
                    self._record_stop_failure()
                    self._record_owner_error("owner_cancellation_failed", exc)
                else:
                    self.feedback_message = f"owner_cancellation_failed: {exc}"
                return True
        if self._closable(task):
            self._drain = (task, identity)
        return True

    def _consume_answer(self) -> py_trees.common.Status:
        """Held: the node's answered request gives its real result."""

        return self._finish(self._update())

    def _abandoned_result(self, request: RunRequest) -> py_trees.common.Status | None:
        """The real result of a sent start the tree stopped (Set TCP)."""

        try:
            task = request.result()
        except (OwnerInvocationHeldError, OwnerInvocationRefusedByPriorityError):
            return None  # Not admitted: nothing started.
        except Exception as exc:
            if isinstance(exc, KernelOperationAdmissionRejectedError):
                if exc.code == "simulation_not_playing":
                    return None  # Not admitted: nothing started.
                code = exc.code
            else:
                code = "owner_request_failed"
            self._record_owner_error(code, exc)
            return py_trees.common.Status.FAILURE
        if task.state is OwnerTaskState.ACTIVE:
            # An accepted control (an operation Reset): the Kernel keeps its
            # effect and the stopped attempt's observation ends, as when
            # `terminate` stops an accepted task.
            if self._identity is not None:
                try:
                    self._port.cancel(task.task_id, self._identity)
                except Exception as exc:
                    self._record_stop_failure()
                    self._record_owner_error("owner_cancellation_failed", exc)
                    return None
            if self.cancellable:
                self._interrupted_reason_code = NODE_INTERRUPTED_REASON
                self._interrupted_message = "the running attempt was stopped"
            return None
        if task.state is OwnerTaskState.SUCCEEDED:
            status = py_trees.common.Status.SUCCESS
        else:
            self._record_owner_failure(task.reason_code, task.message, task.details)
            status = py_trees.common.Status.FAILURE
        if task.settlement_proven and self._identity is not None:
            port, task_id, identity = self._port, task.task_id, self._identity
            if self._sender is not None:
                # Off the tick (OD-380 D2), also after the run's sender
                # closed: the node ends once it is answered. Its outcome is
                # not read: a refused one stays with the port for Reset.
                self._abandoned_ack = (
                    self._sender.submit(
                        lambda: port.acknowledge(task_id, identity), withdrawable=False
                    ),
                    status,
                )
                return None
            try:
                port.acknowledge(task_id, identity)
            except Exception:
                pass  # The port keeps it for Reset, as for any refused acknowledgement.
        return status

    def settle_abandoned_request(self) -> bool:
        """Forced Program End: the sent request of the stopped attempt ends
        it with its real result, never `interrupted`. True once nothing sent
        is outstanding. A sent operation start or settle step is taken once
        answered, as the tick that read it did in base (`_continue_stopped`).
        A start followed after a re-entry is cancelled once answered."""

        if not super().settle_abandoned_request():  # A written value's answer.
            return False
        if not self._continue_stopped():
            return False
        if self._follow_start is not None and not self._take_follow_start():
            return False
        request = self._abandoned
        if request is not None:
            if not request.answered:
                return False
            self._abandoned = None
            status = self._abandoned_result(request)
            if status is not None:
                self.status = status
        if self._abandoned_ack is not None:
            ack, status = self._abandoned_ack
            if not ack.answered:
                return False
            self._abandoned_ack = None
            self.status = status
        return True

    def _update(self) -> py_trees.common.Status:
        if (
            self._drain is not None or self._follow_start is not None
        ) and not self.drain_invalidated():
            # The abandoned attempt's operation still settles; a new Execute
            # on that robot would be refused while it stays resident.
            return py_trees.common.Status.RUNNING
        if self._wait_for_admission_release():
            return py_trees.common.Status.RUNNING
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        if self._phase is _TaskPhase.SUBMIT:
            stopped = self._start_stopped_before_sent()
            if stopped is not None:
                return stopped
        if self._phase is _TaskPhase.SUBMIT and self._request is None:
            gate = self._mock_simulation_ready()
            if gate is not None:
                return gate
        try:
            if self._phase is _TaskPhase.SUBMIT:
                started = self._sent_start()
                if started is None:
                    return py_trees.common.Status.RUNNING
                self._task = started
                self._phase = _TaskPhase.OBSERVE
                if self._require_initial_running:
                    return py_trees.common.Status.RUNNING
            elif self._task is not None and self._task.state is OwnerTaskState.ACTIVE:
                try:
                    observed = self._observed(self._task.task_id)
                except OwnerTaskObservationUnavailableError:
                    # Keep the accepted snapshot and retry only its exact task
                    # identity on a later tick. Submission and acknowledgement
                    # remain one-shot.
                    return py_trees.common.Status.RUNNING
                if observed is None:
                    return py_trees.common.Status.RUNNING  # Its read is on the sender.
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
        except PoseSourceLatchError as error:
            self._record_pose_source_latch_failure(error)
            return py_trees.common.Status.FAILURE
        except Exception as exc:
            if (
                isinstance(exc, KernelOperationAdmissionRejectedError)
                and exc.code == "simulation_not_playing"
            ):
                # Never a node failure; the run owner handles
                # it as the operator's Stop and the node waits for Run.
                self._task = None
                self._phase = _TaskPhase.SUBMIT
                self._hold_for_simulation(exc)
                return py_trees.common.Status.RUNNING
            if isinstance(exc, KernelOperationAdmissionRejectedError) and (
                self._node_id == "motion.set_tcp"
                or exc.code in {"rule_holds", "cartesian_path_moving_takeover_refused"}
            ):
                # A rule holding its scope refuses the motion: the node fails
                # at once with `rule_holds` (its `rule_id` in the details) and
                # the tree handles it. A Cartesian path that would take over a
                # moving robot fails the same way (OD-347); the running motion
                # continues.
                self._record_owner_error(exc.code, exc)
            else:
                self._record_owner_error("owner_request_failed", exc)
            return py_trees.common.Status.FAILURE
        if self._task is None:
            return py_trees.common.Status.FAILURE
        return self._settle(self._task)

    def _settle_request_while_stopped(self) -> bool:
        """An operator Stop holds the node, or a Halt froze the run: a
        request not sent yet is withdrawn (the node stays where it is and
        sends it when it is ticked again); a sent one completes. True while
        the node still has a request: then nothing else is observed."""

        request = self._request
        if request is None:
            return False
        if request.withdraw():
            # One a Program Stop withdrew ends as that Stop decides
            # (`_start_stopped_before_sent`); a Halt's is sent again.
            self._start_stopped = self._start_stopped or request.withdrawn_by_program_stop
            self._request = None
            return False
        return True

    def observe_while_operator_stop_held(self) -> None:
        """Refresh only an already accepted task while its subtree is held."""

        held = self._held_write_result()
        if held is not None:
            # Its written output or outcome ends it once the host answered.
            if held is not py_trees.common.Status.RUNNING:
                self.stop(held)
            return
        if self._settle_request_while_stopped():
            assert self._request is not None
            if self._request.answered:
                status = self._after_write(self._consume_answer())
                if status is not py_trees.common.Status.RUNNING:
                    self.stop(status)
            return
        if self._task is None or self._identity is None or self.status in {
            py_trees.common.Status.SUCCESS, py_trees.common.Status.FAILURE}:
            return
        try:
            if self._task.state is OwnerTaskState.ACTIVE:
                observed = self._observed(self._task.task_id)
                if observed is None:
                    return  # Its read is on the run's sender.
                if observed.task_id != self._task.task_id:
                    self._record_owner_failure(
                        "owner_task_identity_changed",
                        "owner observation selected another task",
                    )
                    return
                if observed.state is not self._task.state and self._operator_stop_state is not None:
                    self._operator_stop_state.note_task_observation_change()
                self._task = observed
            status = self._after_write(self._finish(self._settle(self._task)))
            if status is not py_trees.common.Status.RUNNING:
                self.stop(status)
        except OwnerTaskObservationUnavailableError:
            return
        except OwnerInvocationHeldError as error:
            self._task = None
            self._phase = _TaskPhase.SUBMIT
            self._hold_before_admission(error)
        except PoseSourceLatchError as error:
            self._record_pose_source_latch_failure(error)
        except Exception as exc:
            self._record_owner_error("owner_request_failed", exc)

    def reconcile_halt_task(self, *, selected_resume: bool = False) -> str:
        """Observe accepted Program-scope work without ticking the tree.

        Halt observes only physical tasks in its existing scope. Selected
        Resume also waits for other accepted owners' work; it does not cancel
        them or pretend the Kernel stopped them.
        """

        if self._settle_request_while_stopped():
            assert self._request is not None
            if not self._request.answered:
                # Sent: the run stays Halting (or the selection held) until
                # its answer.
                return "active"
            if not selected_resume and not self._request_opens_operation:
                # Answered: the node returns it on its first tick after Execute.
                return "unaffected"
            # A selected Resume, or an operation start: the accepted
            # operation is observed like one started inside the tick.
            self.observe_while_operator_stop_held()
            if self._request_opens_operation and self._request is None:
                self.observe_while_operator_stop_held()  # Its task, now read.
            return self._control_state(self._task)
        if not selected_resume and self._owner_port_name != "kernel_operations":
            return "unaffected"
        if self._drain is not None or self._follow_start is not None:
            self.drain_invalidated()
        if self._task is None:
            return self._control_state(None)
        self.observe_while_operator_stop_held()
        return self._control_state(self._task)

    @property
    def has_active_task(self) -> bool:
        return self._control_state(self._task) == "active"

    def observe_control_state(self) -> str:
        """Read status without settling tasks, acknowledging or writing outputs.
        Not a tick's read: a read the observation needs is made here."""
        task = self._task
        if task is not None and task.state is OwnerTaskState.ACTIVE:
            try:
                try:
                    observed = self._port.observe(task.task_id)
                except OwnerTaskReadRequiredError as needed:
                    observed = needed.read()
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
        if (
            self._drain is not None
            or self._request is not None
            or self._follow_start is not None
            or self._settle_request is not None
            or self._stopped_settling
        ):
            # An abandoned attempt's operation is still being drained (or
            # its start answered), or a request is on the run's sender.
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
            and assessment.get("recovery_class") == PROGRAM_MOTION_STOP_RECOVERY_CLASS
        ):
            return "held"
        if task.state is OwnerTaskState.SUCCEEDED or (
            task.state is OwnerTaskState.FAILED and task.settlement_proven
        ):
            return "settled"
        return "unrecoverable"

    @property
    def has_pending_drain(self) -> bool:
        return (
            self._drain is not None
            or self._follow_start is not None
            or self._stopped_settling
        )

    @property
    def settling_after_stop(self) -> bool:
        """A Stop ended this attempt while it still takes its sent request
        (`_continue_stopped`): its result is not decided yet."""

        return self._stopped_settling

    def drain_invalidated(self) -> bool:
        """Close the operation of an attempt the tree abandoned (INVALID),
        once its Stop settled. True when nothing is left to drain.

        Success is acknowledged as usual; an operation that is not eligible
        to close (a fault, unknown effects) stays retained for Reset. A
        stopped attempt's sent start or settle step is taken first
        (`_continue_stopped`); a start followed after a re-entry is awaited:
        once answered, what it started is cancelled and drained from the
        next tick, as after a stop inside the tick.
        """

        if not self._continue_stopped():
            return False
        if self._follow_start is not None:
            if not self._take_follow_start():
                return False
            if self._drain is not None:
                return False
        if self._drain is None:
            return True
        task, identity = self._drain
        settling = self._settle_request
        if settling is not None and settling[0] != "observe" and settling[1] == task.task_id:
            # Its acknowledgement or close is on the run's sender: decided
            # on `task`, the observation that sent it.
            observed = task
        else:
            try:
                observed = self._observed(task.task_id)
            except OwnerTaskObservationUnavailableError:
                return self._drain_pending(task, identity)
            except Exception:
                # No longer retained (an accepted recovery or Reset retired it).
                self._drain = None
                return True
            if observed is None:
                return False  # Its read is on the run's sender.
            if observed.state is OwnerTaskState.ACTIVE:
                self._drain = (observed, identity)
                return False
        if observed.state is OwnerTaskState.SUCCEEDED or observed.settlement_proven:
            answered = self._sent_acknowledge(observed.task_id, identity)
        elif _close_eligible(observed):
            answered = self._sent_close(observed.task_id, identity)
        elif not _close_decidable(observed):
            return self._drain_pending(observed, identity)
        else:
            self._drain = None  # Not eligible to close: retained for Reset.
            return True
        if answered is None:
            self._drain = (observed, identity)  # On the run's sender.
            return False
        try:
            answered.result()
        except OwnerTaskSettlementPendingError:
            self._drain = (observed, identity)
            return False
        except Exception:
            pass  # Refused or impossible: retained for Reset, as before.
        self._drain = None
        return True

    def _drain_pending(self, task: OwnerTaskSnapshot, identity: NodeAttemptIdentity) -> bool:
        """The drained operation's assessment is not yet decidable: reread it
        on later ticks (the same wait as `_await_settled_assessment`)."""

        self._drain = (task, identity)
        return False

    def terminate(self, new_status: py_trees.common.Status) -> None:
        if self._stop_deferred(new_status):
            return  # The attempt takes its sent request first.
        if not self._abandon_write(new_status):  # Else its written value ends it.
            self._stop_attempt(new_status)
        self._forget_settling()

    def _settling_on_sender(self) -> bool:
        """A settle step of this attempt's terminal task is on the run's
        sender: one base made inside the tick that saw the task end (its
        read, acknowledgement, close or grasp read; the operator-Stop scope
        read its settling waits for)."""

        settling = self._settle_request
        return (
            settling is not None and not settling[3] and not settling[2].withdrawn
        ) or self._stop_scope_read is not None

    def _stop_deferred(self, new_status: py_trees.common.Status) -> bool:
        """A Stop (INVALID) of a running attempt whose sent operation start
        or settle step is on the run's sender: in base that request and the
        tick reading its answer were one, so the Stop met what that tick
        left. The attempt keeps the request and goes on off the tree
        (`_continue_stopped`): True then, decided once per Stop. A start not
        sent yet is withdrawn and never sent (OD-213 part 3)."""

        if self._stopped_settling:
            return True
        if (
            self._applying_stop
            or new_status is not py_trees.common.Status.INVALID
            or self.status is not py_trees.common.Status.RUNNING
            or self._held_write is not None
        ):
            return False
        request = self._request
        if request is not None:
            if not self._request_opens_operation or request.withdraw():
                return False  # `_stop_attempt` decides (withdrawn, or atomic).
        elif not self._settling_on_sender():
            return False
        self._stopped_settling = True
        return True

    def _stopped_requests(self) -> tuple[Any, ...]:
        """What a stopped attempt's next step waits for: its sent start,
        settle step, scope read or written value's publication."""

        settling = self._settle_request
        held = self._held_write
        return tuple(
            request
            for request in (
                self._request,
                None if settling is None else settling[2],
                self._stop_scope_read,
                None if held is None else held[0],
            )
            if request is not None
        )

    def _continue_stopped(self) -> bool:
        """The stopped attempt (`_stop_deferred`) goes on as base's tick did
        once its request is answered: it ends with its real result (never
        `interrupted`), or, RUNNING with nothing new sent, the Stop applies
        as to a running attempt (`interrupted`, its operation cancelled and
        drained). Each further settle step it sends is awaited the same way.
        True once nothing is left."""

        if not self._stopped_settling:
            return True
        before = self._stopped_requests()
        if any(not request.answered for request in before):
            return False
        try:
            status = self.update()
        except Exception:
            # A failed operator-Stop scope read (base: it failed the tick):
            # the Stop already ends the run; it applies to the attempt.
            status = py_trees.common.Status.RUNNING
            before = self._stopped_requests()
        if status is py_trees.common.Status.RUNNING:
            if any(
                not request.answered or all(request is not old for old in before)
                for request in self._stopped_requests()
            ):
                return False  # Its next step is on the run's sender.
            self._stopped_settling = False
            # The Stop now meets the running attempt, as it did in base.
            self.status = py_trees.common.Status.RUNNING
            self._applying_stop = True
            try:
                self.stop(py_trees.common.Status.INVALID)
            finally:
                self._applying_stop = False
            return True
        self._stopped_settling = False
        self._clear_interruption()
        self.stop(status)  # Its own end, as base's tick ended it.
        return True

    def _stop_attempt(self, new_status: py_trees.common.Status) -> None:
        request, self._request = self._request, None
        if request is not None and not request.withdraw():
            if self._request_opens_operation:
                # A sent motion or gripper start the Stop did not wait for
                # (`_stop_deferred`): the attempt is stopped (`interrupted`)
                # and the run's tick takes the answer, then stops what it
                # started (`drain_invalidated`).
                self._follow_start = (request, self._identity)
                self._record_interruption(new_status)
                return
            # Sent on the run's sender: the atomic step completes and its
            # real result ends this attempt, never `interrupted` (OD-213).
            if new_status is py_trees.common.Status.INVALID:
                self._abandoned = request
            return
        # Halt, Stop or cancel of a running attempt: a cancellable node
        # reports `interrupted`, including a held or not yet admitted one,
        # once the stop is confirmed.
        self._record_interruption(new_status)
        if new_status is not py_trees.common.Status.INVALID:
            return
        self._stop_task()

    def _stop_task(self) -> None:
        """The Stop of a running attempt's accepted task: cancelled while
        active, then drained once its Stop settled."""

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
            self._phase = _TaskPhase.DONE


class _InlineRequest:
    """A request made at once inside the tick, read like a sent one (a node
    built without its run's sender)."""

    withdrawn = False
    answered = True

    def __init__(self, call: Callable[[], Any]) -> None:
        self._result: Any = None
        self._error: Exception | None = None
        try:
            self._result = call()
        except Exception as error:
            self._error = error

    def withdraw(self) -> bool:
        return False

    def result(self) -> Any:
        if self._error is not None:
            raise self._error
        return self._result


class PickPlacePlanningAdapter(OwnerTaskAdapter):
    """Pick/place planning as a planning job on the execution host
    (OD-213 (1)).

    The node sends the plan on its run's sender and is RUNNING: the host
    answers at once with the job. On every tick it reads the job's state
    from the pushed stream copy (memory only; RUNNING while the copy is
    unavailable or does not list the job yet). Once the job is done, failed
    or cancelled it reads the job once and releases it (again on the
    sender), and the result ends the node as before: SUCCESS with the plan,
    FAILURE `no_feasible_candidate`, or FAILURE with the host's typed code.

    A Halt, a hold of this node by a Stop, a branch halt and Forced Program
    End cancel this node's job only (straight to the host's control socket);
    a request still queued on the sender is withdrawn instead. A stopped
    node reports `interrupted`. A frozen or held node plans again, from the
    start, when it is ticked after Execute or Run.

    What a stopped attempt leaves (its sent request, its job) is followed to
    its end by the run's tick (`drain_invalidated`): the job is cancelled
    when its acceptance arrives and released once it ended, and a read the
    host answered is acknowledged, so neither the host's job table nor the
    port keeps it. One such follow-up per node: the next attempt sends its
    plan only once the follow-up is empty, so the node holds at most one job
    on the host.
    """

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        # The sent job, the step of `_request` ("send" or "finish"), and the
        # job this node cancelled (it plans again after it).
        self._job: Any = None
        self._step: str | None = None
        self._cancelled_job_id: str | None = None
        # A stopped attempt's follow-up: its request ("send", "finish" or
        # "release") with the attempt identity, and its job still to release.
        self._follow_request: Any = None
        self._follow_step: str | None = None
        self._follow_identity: NodeAttemptIdentity | None = None
        self._follow_job: Any = None

    # ---- the job's steps -------------------------------------------------
    def _submit(self, step: str, call: Callable[[], Any]) -> None:
        self._step = step
        self._request = (
            _InlineRequest(call) if self._sender is None else self._sender.submit(call)
        )

    def _submit_send(self) -> None:
        assert self._identity is not None and self._owner_inputs is not None
        port, node_id = self._port, self._node_id
        identity, inputs = self._identity, self._owner_inputs
        # A job this attempt cancelled (Halt, hold) ended: it is released
        # first, which frees its name for the new plan.
        stale, self._cancelled_job_id = self._cancelled_job_id, None
        sender = self._sender

        def send() -> Any:
            if stale is not None:
                port.release_planning(stale)  # type: ignore[attr-defined]
            job = port.send_planning(node_id, identity, inputs)  # type: ignore[attr-defined]
            if sender is not None:
                # Its pushed state changes wake the run's tick (S2).
                sender.watch_planning_job(job.job_id)
            return job

        self._submit("send", send)

    def _sent_start(self) -> OwnerTaskSnapshot | None:
        request = self._request
        if request is not None and request.withdrawn:
            # A Halt or Stop withdrew it before it was sent: it is made
            # again below (a send while no job is known, else the read).
            self._request = request = None
        if request is not None:
            if not request.answered:
                return None
            self._request = None
            answer = request.result()  # What the step raised fails the node.
            if self._step == "send":
                self._job = answer
            else:
                self._job = None
                if answer is not None:
                    return answer
                # Cancelled by this node's own cancel: plan again.
                self._submit_send()
                return None
        if self._job is None:
            if self.has_pending_drain:
                # The previous attempt's job is still on the host: one job
                # per node, so this plan waits until it is released.
                return None
            self._submit_send()
            return None
        try:
            state = self._port.planning_job_state(self._job.job_id)  # type: ignore[attr-defined]
        except KernelOperationObservationUnavailableError:
            return None  # The stream copy is unavailable: RUNNING.
        if state not in PLANNING_JOB_FINAL_STATES:
            return None
        if self._cancelled_job_id == self._job.job_id:
            # This node cancelled it (Halt, hold): plan again from the start.
            self._job = None
            self._submit_send()
            return None
        assert self._identity is not None and self._owner_inputs is not None
        port, job = self._port, self._job
        identity, inputs = self._identity, self._owner_inputs
        self._submit(
            "finish",
            lambda: port.finish_planning(job, identity, inputs),  # type: ignore[attr-defined]
        )
        return None

    def _sent_job(self) -> Any:
        """The job this attempt has sent, also from an answered send not yet
        read by a tick; None when none is known."""

        if self._job is not None:
            return self._job
        request = self._request
        if self._step == "send" and request is not None and request.answered:
            try:
                return request.result()
            except Exception:
                return None  # The tick reads the failure.
        return None

    def _cancel_job(self, job: Any) -> bool:
        """Cancel this node's job only, on the control socket. True once the
        host took the cancel (or the job was cancelled before)."""

        if job is None or self._cancelled_job_id == job.job_id:
            return True
        # Asked once: the node plans again after this job ends either way.
        self._cancelled_job_id = job.job_id
        try:
            self._port.cancel_planning(job.job_id)  # type: ignore[attr-defined]
        except Exception as exc:
            self.feedback_message = f"planning cancel failed: {exc}"
            return False
        return True

    def _stop_planning(self) -> str:
        """A Halt froze the run or a Stop holds this node: a queued request
        is withdrawn, the job is cancelled. "active" only while a sent
        request is unanswered (its job is not known yet)."""

        request = self._request
        if request is not None and request.withdraw():
            self._request = None
            request = None
        if request is not None and not request.answered:
            return "active"
        if self._phase is _TaskPhase.SUBMIT:
            self._cancel_job(self._sent_job())
        return "unaffected"

    # ---- a stopped attempt's follow-up -----------------------------------
    @property
    def has_pending_drain(self) -> bool:
        return (
            super().has_pending_drain
            or self._follow_request is not None
            or self._follow_job is not None
        )

    def drain_invalidated(self) -> bool:
        """The run's tick (and the node's own): follow what a stopped
        attempt left, memory only apart from the cancel. True when nothing
        is left."""

        drained = super().drain_invalidated()
        return self._follow_up(run_ended=False) and drained

    def _follow_up(self, *, run_ended: bool) -> bool:
        request = self._follow_request
        if request is not None:
            if request.withdrawn:
                # A queued release a Halt withdrew: made again below.
                self._follow_request = None
            elif not request.answered:
                return False
            else:
                self._follow_request = None
                step, identity = self._follow_step, self._follow_identity
                try:
                    answer = request.result()
                except Exception:
                    # Not accepted (send), or the read failed (finish: the
                    # job is still to release), or the release failed.
                    answer = None
                    if step == "release":
                        self._follow_job = None  # Program Reset releases it.
                else:
                    if step == "send" and answer is not None:
                        # Its own job only; the current attempt's cancel
                        # mark is not touched.
                        try:
                            self._port.cancel_planning(answer.job_id)  # type: ignore[attr-defined]
                        except Exception as exc:
                            # Not cancelled: it plans to its end, then is
                            # released below.
                            self.feedback_message = f"planning cancel failed: {exc}"
                        self._follow_job = answer
                    elif step == "finish":
                        # The read released the job; its task leaves the port.
                        self._follow_job = None
                        if answer is not None and identity is not None:
                            try:
                                self._port.acknowledge(answer.task_id, identity)
                            except Exception:
                                pass  # Not retained (already gone).
                    elif step == "release":
                        self._follow_job = None
        job = self._follow_job
        if job is None:
            return True
        if run_ended:
            # The run's sender is closed: the Program Reset releases the job.
            self._follow_job = None
            return True
        try:
            state = self._port.planning_job_state(job.job_id)  # type: ignore[attr-defined]
        except KernelOperationObservationUnavailableError:
            return False
        if state not in PLANNING_JOB_FINAL_STATES:
            return False  # Its cancel still ends it (or it is not pushed yet).
        if self._sender is None:
            self._port.release_planning(job.job_id)  # type: ignore[attr-defined]
            self._follow_job = None
            return True
        port = self._port
        self._follow_step = "release"
        self._follow_request = self._sender.submit(
            lambda: port.release_planning(job.job_id)  # type: ignore[attr-defined]
        )
        return False

    def _leave_follow_up(self, request: Any, step: str | None, job: Any) -> None:
        """The attempt stops: what it left goes to the follow-up (empty now:
        a new attempt sends nothing while it is not)."""

        self._follow_request = request
        self._follow_step = step
        self._follow_identity = self._identity
        self._follow_job = job

    # ---- Halt, hold, branch halt, Forced Program End --------------------
    def reconcile_halt_task(self, *, selected_resume: bool = False) -> str:
        if self._phase is not _TaskPhase.SUBMIT:
            return super().reconcile_halt_task(selected_resume=selected_resume)
        return self._stop_planning()

    def observe_while_operator_stop_held(self) -> None:
        if self._phase is not _TaskPhase.SUBMIT:
            super().observe_while_operator_stop_held()
            return
        self._stop_planning()

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._job = None
        self._step = None
        self._cancelled_job_id = None

    def settle_abandoned_request(self) -> bool:
        """Forced Program End: a stopped attempt's sent request completes
        (its job is cancelled); its job is released by the Program Reset.
        True once nothing sent is outstanding (nor a written output's
        publication, which ends it with its result)."""

        if not PublishedWriteHold.settle_abandoned_request(self):
            return False
        if not self._continue_stopped():  # A settle step of its finished plan.
            return False
        return self._follow_up(run_ended=True)

    def terminate(self, new_status: py_trees.common.Status) -> None:
        if self._phase is not _TaskPhase.SUBMIT:
            super().terminate(new_status)
            return
        request, self._request = self._request, None
        job = self._job
        # A read queued for a job that ended: nothing to cancel, only to release.
        ended = self._step == "finish"
        if request is not None and not request.withdraw():
            if not request.answered:
                # Sent: followed until its answer (then its job is
                # cancelled, or its read acknowledged).
                self._leave_follow_up(request, self._step, job)
                self._job = None
                self._record_interruption(new_status)
                return
            if self._step == "send":
                try:
                    job = request.result()
                except Exception:
                    job = None  # Not accepted: nothing plans.
            elif self._step == "finish":
                # Answered, not read by a tick: its task leaves the port.
                self._leave_follow_up(request, "finish", None)
                self._job = None
                self._record_interruption(new_status)
                return
        self._job = None
        if job is None:
            self._record_interruption(new_status)
            return
        cancelled = ended or self._cancel_job(job)
        # Released by the follow-up once it ended (finished or cancelled).
        self._leave_follow_up(None, None, job)
        if not cancelled:
            # The host did not take the cancel: the stop is not confirmed.
            self._record_stop_failure()
            self._record_node_failure(
                "owner_cancellation_failed", self.feedback_message or "planning cancel failed"
            )
            return
        self._record_interruption(new_status)


@dataclass(frozen=True, slots=True)
class _PendingRetry:
    cause: str
    delay_s: float
    budget: int
    code: str | None
    message: str | None


_PREEMPTION_OUTCOMES: Final = {"preempted": "preempted", "superseded_unsent": "superseded"}


class MotionTaskAdapter(OwnerTaskAdapter):
    """A motion node retries its own eligible failed attempt.

    The Kernel alone decides eligibility. On a failure this adapter decides
    first: an eligible attempt with budget left is closed, waits on the
    Program clock, passes the owner's motion-attention check and submits a
    fresh Execute; otherwise the failure is held or closed as for any owner
    task. With the run's sender the check and then the fresh start leave the
    tick one after the other (OD-380 D2); a Halt, a Stop hold or the node's
    end drops a check not read yet, and the node checks again when it is
    ticked (no start without a check read on its own tick). Any operator release of a hold over the node restarts the budget.
    A logically preempted (or priority-refused) node ends with its literal
    `preempted_result`; every end writes the outcome to the node's
    system-owned key `node_outcome.<instance_id>`.
    """

    _START_OPENS_OPERATION: ClassVar[bool] = True

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
        self._takeover_noted = False
        # The automatic retry's motion-attention check on the run's sender.
        self._attention: RunRequest | None = None

    @property
    def preempted(self) -> bool:
        """This attempt was logically preempted, superseded unsent or refused
        by priority: its `preempted_result` goes straight to the tree, never held for
        intervention nor retried automatically (maintainer decision)."""

        return self._preempted

    def _begin_attempt(self, *, restart_series: bool) -> None:
        if restart_series:
            self._retries_used = 0
        self._drop_attention()
        self._retry = None
        self._outcome = None
        self._outcome_written = False
        self._preempted = False
        self._takeover_noted = False
        super()._begin_attempt(restart_series=restart_series)
        # A new attempt has no result yet: its outcome key is `none` again
        # until it ends (`_finish`).
        # Every motion node's key is declared by the Program.
        identity = self._identity
        self._input_resolver.value_store.reset_node_outcome(
            self._node_instance_id,
            metadata=(
                {}
                if identity is None
                else {
                    "node_id": self._node_id,
                    "node_instance_id": self._node_instance_id,
                    "program_run_id": identity.program_run_id,
                    "node_attempt_id": identity.node_attempt_id,
                }
            ),
        )

    def restart_retry_series(self) -> None:
        """The operator released a hold over this node: full budget again."""

        self._retries_used = 0

    def _drop_attention(self) -> None:
        """A check not read yet is dropped (a sent one completes, unread)."""

        if self._attention is not None:
            self._attention.withdraw()
            self._attention = None

    def _update(self) -> py_trees.common.Status:
        if self._phase is not _TaskPhase.BACKOFF:
            return super()._update()
        retry = self._retry
        identity = self._identity
        assert retry is not None and identity is not None  # Set with BACKOFF.
        if self._retry_time.elapsed() < retry.delay_s:
            return py_trees.common.Status.RUNNING
        attention = self._attention
        try:
            check = self._motion_attention
            if check is None:
                raise RuntimeError("automatic motion retry has no motion attention check")
            if attention is None or attention.withdrawn:
                # The check an operator Retry runs before any motion effect
                # (the Kernel's refusal keeps its own code), on the run's
                # sender; the fresh start follows once it is read.
                request_key = program_key(
                    identity.program_run_id, identity.node_attempt_id, Phase.AUTOMATIC_RETRY
                )
                node_instance_id = identity.node_instance_id
                if self._sender is None:
                    check(node_instance_id, request_key)
                else:
                    self._attention = self._sender.submit(
                        lambda: check(node_instance_id, request_key)
                    )
                    return py_trees.common.Status.RUNNING
            elif not attention.answered:
                return py_trees.common.Status.RUNNING
            else:
                self._attention = None
                attention.result()
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
        # A held node waits: no fresh attempt starts under a Stop; its check
        # is made again after Run or Resume.
        if self._phase is not _TaskPhase.BACKOFF:
            super().observe_while_operator_stop_held()
        else:
            self._drop_attention()

    def reconcile_halt_task(self, *, selected_resume: bool = False) -> str:
        if self._phase is _TaskPhase.BACKOFF:
            # A Halt drops the check: it is made again after Execute.
            self._drop_attention()
        return super().reconcile_halt_task(selected_resume=selected_resume)

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._drop_attention()
        super().terminate(new_status)

    def _note_takeover_fallback(self, task: OwnerTaskSnapshot) -> None:
        """History only: this newcomer's takeover took a fallback path (not a
        moving replacement). Once per attempt; never changes the outcome."""

        takeover = task.metadata.get("motion_takeover")
        if (
            self._takeover_noted
            or not isinstance(takeover, Mapping)
            or takeover.get("path") == "moving_replacement"
        ):
            return
        self._takeover_noted = True
        if self._audit_sink is None:
            return
        cause = takeover.get("cause")
        cause = cause if isinstance(cause, Mapping) else {}
        details = cause.get("details")
        stage = details.get("failure_stage") if isinstance(details, Mapping) else None
        self._audit_sink(
            {
                "kind": "program_node_takeover_fallback",
                "node_instance_id": self._node_instance_id,
                "incumbent_operation_id": takeover.get("incumbent_operation_id"),
                "path": takeover.get("path"),
                "code": cause.get("code"),
                "message": cause.get("message"),
                # Where the time ran out, when the Kernel names it.
                "failure_stage": stage if isinstance(stage, str) and stage else None,
                "preparation_ms": takeover.get("elapsed_since_acceptance_ns", 0) / 1e6,
                "budget_ms": takeover.get("preparation_budget_ns", 0) / 1e6,
                # The planning cap that applied: the deadline a planning timeout trips.
                "planning_cap_ms": takeover.get("planning_budget_ns", 0) / 1e6,
            }
        )

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        self._note_takeover_fallback(task)
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
        """This attempt's automatic retry budget.

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
            receipt = self._input_resolver.value_store.write_node_outcome(
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
        if receipt is not None:
            self._written = receipt  # The result waits for the host's answer.
        return status


class InterruptMotionTaskAdapter(MotionTaskAdapter):
    """`motion.move_until_signal` / `motion.probe_search`.

    The terminal operation's typed rule result (`metadata["interrupt_operation"]`,
    the operation projection with its participants' `io_rule_outcomes`) is
    mapped by `interrupt_outcome.map_interrupt_result` to the node result, the
    node outcome (`signal_trip` / `no_signal_trip` / `failed`) and one history
    row. Each covered participant's `stop` object in that operation read
    names the stage that contained it and the mode of the Stop plan that
    ran (an interrupt plan is judged by the rule's threshold); the robot's
    qualified Stop chain is
    `interrupt_stop_chain` (per participant: the Targets motion command's
    `stop_response` stages of the binding the operation used). While a
    covered participant is still stopping the node waits. An ordinary Stop
    (no interrupt terminal) or an operator-attributed Stop keeps the existing
    motion handling.
    """

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._interrupt: InterruptNodeResult | None = None
        self._stop_waiting = False

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._interrupt = None
        self._stop_waiting = False

    def _retry_limit(self) -> int:
        # Search and contact moves have no automatic retry;
        # a source loss is never retried either.
        return 0

    @staticmethod
    def _stop_pending(task: OwnerTaskSnapshot) -> bool:
        operation = task.metadata.get("interrupt_operation")
        return isinstance(operation, Mapping) and interrupt_stop_pending(operation)

    def _await_participant_stops(
        self, task: OwnerTaskSnapshot
    ) -> tuple[py_trees.common.Status | None, OwnerTaskSnapshot]:
        """A covered participant may still be stopping when the operation is
        terminal. While one is, the node stays RUNNING and re-observes the
        terminal task once per later tick (one operation read each, on the
        run's sender: `_observed`); the wait ends when every covered
        participant is stopped or failed, or when the node is halted."""

        if not self._stop_pending(task):
            return None, task
        if self._stop_waiting:
            try:
                observed = self._observed(task.task_id, settling=True)
            except OwnerTaskObservationUnavailableError:
                # Stopped and unreadable: it waits, as in `_failed_read`.
                return py_trees.common.Status.RUNNING, task
            except Exception as exc:
                self._record_owner_error("owner_request_failed", exc)
                return py_trees.common.Status.FAILURE, task
            if observed is None:
                return py_trees.common.Status.RUNNING, task  # Read unanswered.
            if observed.task_id != task.task_id:
                self._record_owner_failure(
                    "owner_task_identity_changed", "owner observation selected another task"
                )
                return py_trees.common.Status.FAILURE, task
            self._task = task = observed
            if not self._stop_pending(task):
                return None, task
        self._stop_waiting = True
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
        return map_interrupt_result(
            self._node_id,
            self._inputs or {},
            operation,
            stop_chain=chain,
        )

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        self._note_takeover_fallback(task)
        if (
            self._phase is not _TaskPhase.OBSERVE
            or self._interrupt is not None
            or task.state is OwnerTaskState.ACTIVE
            or isinstance(task.metadata.get("preemption"), Mapping)
        ):
            return super()._settle(task)
        waiting, task = self._await_participant_stops(task)
        if waiting is not None:
            return waiting
        if _terminal_cleanup_pending(task):
            # Decide (close, hold or fail) on the settled assessment: a held
            # failure keeps its Retry and Continue offers.
            waiting, task = self._await_settled_assessment(task)
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


class _PneumaticRoute:
    """A gripper start's answer that started nothing yet: the target is
    pneumatic, so its valve writes come first (`pneumatic_plan`)."""

    __slots__ = ("route",)

    def __init__(self, route: Mapping[str, Any]) -> None:
        self.route = route


class GripperCommandAdapter(OwnerTaskAdapter):
    """Consume real delivery, then dwell on the run's existing Program clock.

    With the run's sender (OD-380 D2) the start leaves the tick with its
    route read: one request reads the captured route (`pneumatic_plan`) and,
    for a target that is not pneumatic, starts the command at once; a
    pneumatic target's valve writes then go one by one, and its logical
    command is the last request.
    """

    _START_OPENS_OPERATION: ClassVar[bool] = True

    def __init__(
        self,
        *,
        program_clock: ProgramClock,
        operator_stop_state: OperatorMotionStopState | None = None,
        operator_stop_hold_scope: str | None = None,
        io_watches: OwnerTaskPort | None = None,
        io_signals: OwnerInvocationPort | None = None,
        valve_sender: RunSender | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(operator_stop_state=operator_stop_state, **kwargs)
        # The run's sender for the valve writes (OD-213): each write leaves
        # the tick like `io.set_output`. None: written inside the tick.
        self._valve_sender = valve_sender
        self._dwell_time = AttemptTime(
            program_clock,
            operator_stop_state=(operator_stop_state if operator_stop_hold_scope is not None else None),
            operator_stop_hold_scope=operator_stop_hold_scope,
        )
        self._dwell_s: float | None = None
        self._timing_captured = False
        self._dwell_started = False
        self._draining_dwell = False
        self._io_watches = io_watches
        self._io_signals = io_signals
        self._pneumatic_route: Mapping[str, Any] | None = None
        self._pneumatic_checked = False
        self._pneumatic_stage = "outputs"
        self._pneumatic_output_index = 0
        self._pneumatic_input_index = 0
        self._pneumatic_watch: OwnerTaskSnapshot | None = None
        # The last attempt's watch release, sent when this attempt began:
        # its watch stays the node's until the release is confirmed.
        self._last_watch_release: RunRequest | None = None
        # A watch release this attempt sent before it ends (a deadline or a
        # failed input step), and how it ends once that is answered.
        self._watch_release: tuple[RunRequest, Callable[[], py_trees.common.Status]] | None = None
        self._pneumatic_drain_id: str | None = None
        # Opt-in grasp confirmation of a Close: its result, decided once.
        self._grasp_status: py_trees.common.Status | None = None

    def _begin_attempt(self, *, restart_series: bool) -> None:
        pending, self._watch_release = self._watch_release, None
        if pending is not None and self._last_watch_release is None:
            # Entered again before its answer: it is this attempt's release.
            self._last_watch_release = pending[0]
        self._drop_pneumatic_watch()
        super()._begin_attempt(restart_series=restart_series)
        self._grasp_status = None
        self._dwell_s = None
        self._timing_captured = False
        self._dwell_started = False
        self._pneumatic_route = None
        self._pneumatic_checked = False
        self._pneumatic_stage = "outputs"
        self._pneumatic_output_index = 0
        self._pneumatic_input_index = 0

    def _release_pneumatic_watch(self) -> bool:
        task, identity = self._pneumatic_watch, self._identity
        if task is None or identity is None or self._io_watches is None:
            return task is None
        try:
            if task.state is OwnerTaskState.SUCCEEDED:
                self._io_watches.acknowledge(task.task_id, identity)
            else:
                self._io_watches.cancel(task.task_id, identity)
        except Exception:
            # The port retains an unconfirmed token for run teardown.
            return False
        self._pneumatic_watch = None
        return True

    def _pneumatic_watch_release(self) -> Callable[[], Any]:
        task, identity, watches = self._pneumatic_watch, self._identity, self._io_watches
        assert task is not None and identity is not None and watches is not None
        release = watches.acknowledge if task.state is OwnerTaskState.SUCCEEDED else watches.cancel
        return lambda: release(task.task_id, identity)

    def _drop_pneumatic_watch(self) -> None:
        """The next attempt begins: the last one's input watch is released
        (one try, as base's). With the run's sender the release leaves the
        tick (OD-380 D2; not withdrawable): the watch stays the node's until
        the release is confirmed (``_last_watch_released``), as when it was
        released inside the tick; an unconfirmed release keeps it, and the
        port keeps its exact token for run teardown."""

        self._last_watch_released()  # An earlier one's answer first.
        if (
            self._sender is None
            or self._pneumatic_watch is None
            or self._identity is None
            or self._io_watches is None
        ):
            self._release_pneumatic_watch()
            return
        if self._last_watch_release is not None:
            return  # Its release is on the sender already.
        self._last_watch_release = self._sender.submit(
            self._pneumatic_watch_release(), withdrawable=False
        )

    def _release_then(
        self, end: Callable[[], py_trees.common.Status]
    ) -> py_trees.common.Status:
        """The attempt ends (a deadline, a failed input step): its watch is
        released, one try, then it ends with ``end()``, as base released it
        inside the tick right before. With the run's sender that try leaves
        the tick (not withdrawable) and the node is RUNNING until its answer
        (`_released_then`); the node's end (`terminate`) tries once more
        while the watch is still kept, as base did."""

        if (
            self._sender is None
            or self._pneumatic_watch is None
            or self._identity is None
            or self._io_watches is None
        ):
            self._release_pneumatic_watch()
            return end()
        self._watch_release = (
            self._sender.submit(self._pneumatic_watch_release(), withdrawable=False),
            end,
        )
        return py_trees.common.Status.RUNNING

    def _released_then(self) -> py_trees.common.Status | None:
        """The attempt's release (`_release_then`): RUNNING until answered,
        then its end; None when none is pending. A confirmed release ends
        the watch; an unconfirmed one keeps it."""

        pending = self._watch_release
        if pending is None:
            return None
        request, end = pending
        if not request.answered:
            return py_trees.common.Status.RUNNING
        self._watch_release = None
        if request.error is None:
            self._pneumatic_watch = None
        return end()

    def _sent_pneumatic_release(self) -> bool | None:
        """The finished input watch's release, on the run's sender
        (``_owner_call``, not withdrawable): None until its answer, then
        whether it was confirmed."""

        assert self._pneumatic_watch is not None
        answered = self._owner_call(
            "pneumatic_release",
            f"pneumatic-watch:{self._pneumatic_watch.task_id}",
            self._pneumatic_watch_release(),
            withdrawable=False,
        )
        if answered is None:
            return None
        try:
            answered.result()
        except Exception:
            # The port retains an unconfirmed token for run teardown.
            return False
        self._pneumatic_watch = None
        return True

    def _last_watch_released(self) -> bool:
        """The last attempt's watch release (``_begin_attempt``): False while
        it is not answered. A confirmed one ends that watch; an unconfirmed
        or withdrawn one keeps it, as a release refused inside the tick."""

        request = self._last_watch_release
        if request is None:
            return True
        if not (request.answered or request.withdrawn):
            return False
        self._last_watch_release = None
        if request.answered and request.error is None:
            self._pneumatic_watch = None
        return True

    def _pneumatic_request_withdrawn(self) -> bool:
        """At the input deadline: no input request is outstanding, or the
        one not sent yet is withdrawn and never sent. False while a sent
        one awaits its answer: its late answer fails the wait as after the
        deadline."""

        settling = self._settle_request
        if settling is None or not settling[0].startswith("pneumatic_"):
            return True
        if settling[2].withdraw():
            self._settle_request = None
            return True
        return False

    def _close_request_key(self, identity: NodeAttemptIdentity) -> bytes:
        task = self._drain[0] if self._drain is not None else self._task
        if task is not None and (
            self._pneumatic_route is not None or task.task_id == self._pneumatic_drain_id
        ):
            return program_key(
                identity.program_run_id, identity.node_attempt_id, Phase.PNEUMATIC_CLOSE
            )
        return super()._close_request_key(identity)

    def _start_task(self) -> OwnerTaskSnapshot:
        if self._pneumatic_route is not None:
            assert self._identity is not None
            return self._port.start(self._node_id, self._identity, self._pneumatic_route["logical"])
        return super()._start_task()

    def _start_call(self) -> Callable[[], Any]:
        assert self._identity is not None and self._owner_inputs is not None
        port, node_id, identity = self._port, self._node_id, self._identity
        if self._pneumatic_route is not None:
            logical = self._pneumatic_route["logical"]
            return lambda: port.start(node_id, identity, logical)
        inputs = self._owner_inputs
        planner = getattr(port, "pneumatic_plan", None)

        def start() -> Any:
            route = planner(identity, inputs) if callable(planner) else None
            if route is not None:
                return _PneumaticRoute(route)
            return port.start(node_id, identity, inputs)

        return start

    def _sent_start(self) -> OwnerTaskSnapshot | None:
        answer = super()._sent_start()
        if not isinstance(answer, _PneumaticRoute):
            return answer
        route = answer.route
        if self._io_signals is None or (route["inputs"] and self._io_watches is None):
            raise ValueError("pneumatic route requires the existing I/O write, watch and read ports")
        self._pneumatic_route = route
        self._pneumatic_checked = True
        return None

    def update(self) -> py_trees.common.Status:
        if self._wait_for_admission_release():
            return py_trees.common.Status.RUNNING
        if (
            self._sender is not None
            and (self._pneumatic_route is None or self._pneumatic_stage == "logical")
        ):
            # The route is read with the start (`_start_call`); a pneumatic
            # route's valve writes begin on the tick that reads it.
            status = super().update()
            if (
                status is py_trees.common.Status.RUNNING
                and self._pneumatic_route is not None
                and self._pneumatic_stage == "outputs"
                # A stopped attempt that took its route writes nothing: its
                # first write was not sent when the Stop came, so it is
                # never sent (OD-213 part 3).
                and not self._stopped_settling
            ):
                return self._write_pneumatic_outputs()
            return status
        if not self._pneumatic_checked and self._identity is not None and self._owner_inputs is not None:
            planner = getattr(self._port, "pneumatic_plan", None)
            try:
                self._pneumatic_route = planner(self._identity, self._owner_inputs) if callable(planner) else None
                if self._pneumatic_route is not None and (
                    self._io_signals is None
                    or (self._pneumatic_route["inputs"] and self._io_watches is None)
                ):
                    raise ValueError("pneumatic route requires the existing I/O write, watch and read ports")
                self._pneumatic_checked = True
            except OwnerInvocationHeldError as error:
                self._hold_before_admission(error)
                return py_trees.common.Status.RUNNING
            except Exception as exc:
                self._record_owner_error("owner_request_failed", exc)
                return py_trees.common.Status.FAILURE
        if self._pneumatic_route is None or self._pneumatic_stage == "logical":
            return super().update()
        if self._pneumatic_stage == "outputs":
            return self._write_pneumatic_outputs()
        return self._update_pneumatic_inputs()

    def _write_pneumatic_outputs(self) -> py_trees.common.Status:
        """Each valve output once through the one write path,
        then the dwell. A failed, unknown or refused write fails the node
        with its code; nothing is resent."""

        assert self._pneumatic_route is not None and self._identity is not None
        assert self._io_signals is not None
        if self.has_pending_drain and not self.drain_invalidated():
            return py_trees.common.Status.RUNNING
        gate = self._mock_simulation_ready()
        if gate is not None:
            return gate
        outputs = self._pneumatic_route["outputs"]
        while self._pneumatic_output_index < len(outputs):
            row = outputs[self._pneumatic_output_index]
            if self._valve_sender is not None:
                # One write at a time on the run's sender, read on a later tick.
                request = self._request
                if request is None or request.withdrawn:
                    signals, identity = self._io_signals, self._identity
                    try:
                        self._request = self._valve_sender.submit(
                            lambda: signals.invoke("io.set_output", identity, OwnerNodeInputs(row))
                        )
                        self._request_opens_operation = False
                    except Exception as exc:
                        self._record_owner_error(_io_reason(exc), exc)
                        return py_trees.common.Status.FAILURE
                    return py_trees.common.Status.RUNNING
                status = self._valve_answer()
                if status is not py_trees.common.Status.SUCCESS:
                    return status
                continue
            self._pneumatic_output_index += 1
            try:
                result = self._io_signals.invoke("io.set_output", self._identity, OwnerNodeInputs(row))
            except Exception as exc:
                self._record_owner_error(_io_reason(exc), exc)
                return py_trees.common.Status.FAILURE
            if not result.succeeded:
                self._record_owner_failure(result.reason_code, result.message, result.details)
                return py_trees.common.Status.FAILURE
        self._dwell_time.start(exclude_held=(self._inputs or {}).get("dwell_while_held", "pause") == "pause")
        self._pneumatic_stage = "watch" if self._pneumatic_route["inputs"] else "dwell"
        return py_trees.common.Status.RUNNING

    def _valve_answer(self) -> py_trees.common.Status:
        """The sent valve write's answer: RUNNING while unanswered, SUCCESS
        when the write succeeded (the next row follows), else FAILURE with
        its code. A failed write is never resent."""

        request = self._request
        assert request is not None
        if not request.answered:
            return py_trees.common.Status.RUNNING
        self._request = None
        self._pneumatic_output_index += 1
        try:
            result = request.result()
        except Exception as exc:
            self._record_owner_error(_io_reason(exc), exc)
            return py_trees.common.Status.FAILURE
        if not result.succeeded:
            self._record_owner_failure(result.reason_code, result.message, result.details)
            return py_trees.common.Status.FAILURE
        return py_trees.common.Status.SUCCESS

    def _consume_answer(self) -> py_trees.common.Status:
        if self._request_opens_operation:
            return super()._consume_answer()  # The start's answer.
        # Held: a written valve keeps the node where it is (the next write
        # waits for Run/Resume); a failed one ends it with its code.
        status = self._valve_answer()
        return py_trees.common.Status.RUNNING if status is py_trees.common.Status.SUCCESS else status

    def _abandoned_result(self, request: RunRequest) -> py_trees.common.Status | None:
        # Forced Program End after a valve write was sent: a failed write
        # ends the node with its code; a written one leaves the gripper
        # command stopped between its steps (`interrupted`).
        try:
            result = request.result()
        except Exception as exc:
            self._record_owner_error(_io_reason(exc), exc)
            return py_trees.common.Status.FAILURE
        if not result.succeeded:
            self._record_owner_failure(result.reason_code, result.message, result.details)
            return py_trees.common.Status.FAILURE
        if self.cancellable:
            # As `_record_interruption` records it when a running attempt stops.
            self._interrupted_reason_code = NODE_INTERRUPTED_REASON
            self._interrupted_message = "the running attempt was stopped"
        return None

    def _finish(self, status: py_trees.common.Status) -> py_trees.common.Status:
        # A required grasp confirmation (the owner names its read on the
        # Close's own task) decides a completed Close once: no evidence fails
        # the node with the typed `grasp_not_confirmed`.
        check = (self._task.metadata.get("grasp_confirmation_check")
                 if self._task is not None and self._task.state is OwnerTaskState.SUCCEEDED else None)
        if status is py_trees.common.Status.SUCCESS and isinstance(check, Mapping):
            if self._grasp_status is None:
                decided = self._confirm_grasp(check)
                if decided is None:
                    return py_trees.common.Status.RUNNING  # Its read is on the sender.
                self._grasp_status = decided
            return self._grasp_status
        return status

    def _confirm_grasp(self, check: Mapping[str, Any]) -> py_trees.common.Status | None:
        """The grasp evidence read leaves the tick on the run's sender
        (OD-380 D2): None until its answer, then the decided result."""

        node_id = check["node_id"]
        try:
            signals, identity, task = self._io_signals, self._identity, self._task
            if signals is None or identity is None or task is None:
                raise ValueError("the grasp evidence read port is unavailable")
            inputs = OwnerNodeInputs(check["parameters"])
            answered = self._owner_call(
                "grasp", f"grasp:{task.task_id}",
                lambda: signals.invoke(node_id, identity, inputs),
                withdrawable=False,
            )
            if answered is None:
                return None
            result = answered.result()
        except Exception as exc:
            self._record_node_failure("grasp_not_confirmed", f"Grasp evidence could not be read: {exc}")
            return py_trees.common.Status.FAILURE
        confirmed = result.succeeded and (
            node_id == "io.check_input" or result.metadata.get("grasp_confirmed") is True)
        if confirmed:
            return py_trees.common.Status.SUCCESS
        source = ("workpiece_grasped input" if node_id == "io.check_input"
                  else "stall position window")
        detail = f" ({result.reason_code}: {result.message})" if result.reason_code else ""
        self._record_node_failure(
            "grasp_not_confirmed", f"Close completed without grasp evidence from the {source}{detail}.")
        return py_trees.common.Status.FAILURE

    def _update_pneumatic_inputs(self) -> py_trees.common.Status:
        """The valve's input confirmation. With the run's sender each watch
        registration, read, release and input check leaves the tick
        (OD-380 D2, ``_owner_call``): the node is RUNNING until its answer,
        read on a later tick. The watch is released at base's points, each
        one try: when it ends, again after a failed step (or a deadline)
        before the node fails (`_release_then`), and at the node's end."""

        assert self._pneumatic_route is not None and self._identity is not None
        required = self._pneumatic_route["inputs"]
        duration = self._pneumatic_route["dwell_s"]
        if not required:
            if self._dwell_time.elapsed() < duration:
                return py_trees.common.Status.RUNNING
            self._pneumatic_stage = "logical"
            return py_trees.common.Status.RUNNING
        if not self._last_watch_released():
            return py_trees.common.Status.RUNNING
        ended = self._released_then()
        if ended is not None:
            return ended
        if (
            duration > 0
            and self._dwell_time.elapsed() >= duration
            and self._pneumatic_request_withdrawn()
        ):
            return self._release_then(self._input_wait_timed_out)
        assert self._io_watches is not None and self._io_signals is not None
        watches, signals, identity = self._io_watches, self._io_signals, self._identity
        try:
            index = self._pneumatic_input_index
            inputs = OwnerNodeInputs(required[index])
            settling = self._settle_request
            if self._pneumatic_stage == "watch":
                if settling is not None and settling[0] == "pneumatic_release":
                    pass  # The ended watch's release is on the sender.
                elif self._pneumatic_watch is None:
                    answered = self._owner_call(
                        "pneumatic_watch", f"pneumatic-input:{index}",
                        lambda: watches.start("io.wait_for_input", identity, inputs),
                    )
                    if answered is None:
                        return py_trees.common.Status.RUNNING
                    self._pneumatic_watch = answered.result()
                else:
                    task_id = self._pneumatic_watch.task_id
                    answered = self._owner_call(
                        "pneumatic_observe", f"pneumatic-watch:{task_id}",
                        lambda: watches.observe(task_id),
                    )
                    if answered is None:
                        return py_trees.common.Status.RUNNING
                    observed = answered.result()
                    if observed.task_id != self._pneumatic_watch.task_id:
                        raise ValueError("pneumatic input watch identity changed")
                    self._pneumatic_watch = observed
                task = self._pneumatic_watch
                if task.state is OwnerTaskState.ACTIVE:
                    return py_trees.common.Status.RUNNING
                released = self._sent_pneumatic_release()
                if released is None:
                    return py_trees.common.Status.RUNNING
                if not released:
                    raise ValueError("pneumatic input watch release was not confirmed")
                if task.state is not OwnerTaskState.SUCCEEDED:
                    self._record_owner_failure(task.reason_code, task.message, task.details)
                    return py_trees.common.Status.FAILURE
                self._pneumatic_input_index += 1
                if self._pneumatic_input_index == len(required):
                    self._pneumatic_input_index = 0
                    self._pneumatic_stage = "confirm"
            else:
                answered = self._owner_call(
                    "pneumatic_check", f"pneumatic-input:{index}",
                    lambda: signals.invoke("io.check_input", identity, inputs),
                )
                if answered is None:
                    return py_trees.common.Status.RUNNING
                result = answered.result()
                if not result.succeeded:
                    if result.reason_code not in {"io_input_not_matched", "io_input_unavailable"}:
                        self._record_owner_failure(result.reason_code, result.message, result.details)
                        return py_trees.common.Status.FAILURE
                    self._pneumatic_stage = "watch"
                else:
                    self._pneumatic_input_index += 1
                    if self._pneumatic_input_index == len(required):
                        self._pneumatic_stage = "logical"
        except OwnerTaskObservationUnavailableError:
            return py_trees.common.Status.RUNNING
        except Exception as exc:
            error = exc

            def failed() -> py_trees.common.Status:
                self._record_owner_error(_io_reason(error), error)
                return py_trees.common.Status.FAILURE

            return self._release_then(failed)
        if duration > 0 and self._dwell_time.elapsed() >= duration:
            return self._release_then(self._input_wait_timed_out)
        return py_trees.common.Status.RUNNING

    def _input_wait_timed_out(self) -> py_trees.common.Status:
        self._record_node_failure("io_wait_timeout", "Pneumatic input confirmation exceeded command_duration_s.")
        return py_trees.common.Status.FAILURE

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        settling = self._settle_request
        if settling is not None and settling[0] == "grasp":
            # Settled: the Close's grasp read is on the run's sender.
            return py_trees.common.Status.SUCCESS
        if self._pneumatic_route is not None:
            # The mapped route already spent the dwell/confirmation interval.
            return OwnerTaskAdapter._settle(self, task)
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

    def _settling_on_sender(self) -> bool:
        # Also a watch release base made inside the tick that ended the watch.
        return super()._settling_on_sender() or self._watch_release is not None

    def _stopped_requests(self) -> tuple[Any, ...]:
        pending = self._watch_release
        return super()._stopped_requests() + (() if pending is None else (pending[0],))

    def terminate(self, new_status: py_trees.common.Status) -> None:
        if self._stop_deferred(new_status):
            return  # The stopped attempt takes its sent request first (base order).
        if self._last_watch_release is None:  # Else that release ends it.
            # Inline, as base's: the node's end (a Stop's control lane).
            self._release_pneumatic_watch()
        if (new_status is py_trees.common.Status.INVALID and self._pneumatic_route is not None
            and self._task is not None):
            self._pneumatic_drain_id = self._task.task_id
        super().terminate(new_status)
        if new_status is py_trees.common.Status.INVALID:
            self._dwell_s = None
            self._dwell_started = False


class OwnerInvocationAdapter(_OwnerAdapter):
    """Issue one owner request per node attempt.

    With the run's sender (OD-213, OD-380 D2: every invocation node) the
    request leaves the tick: the node is RUNNING until its answer and reads
    it on a later tick. The request is an atomic action. A Halt, or an
    operator Stop that holds the node, withdraws it while it is not sent: it
    is never sent, and the node stays RUNNING where it is and sends it when
    it is ticked again. Once sent it completes: the node returns its real
    result, never `interrupted`, and a Halt or Forced Program End settles only
    after that answer. Without a sender the call is made inside the tick.
    """

    def __init__(self, *, port: OwnerInvocationPort,
                 operator_stop_state: OperatorMotionStopState | None = None,
                 run_sender: RunSender | None = None, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._port = port
        self._invoked = False
        self._status = py_trees.common.Status.INVALID
        self._pending_observation = None
        self._operator_stop_state = operator_stop_state
        self._sender = run_sender
        # The request on the run's sender: the node's effect, or a read of
        # its unconfirmed outcome (``_request_effect`` False).
        self._request: RunRequest | None = None
        self._request_effect = False
        # A sent effect of an attempt the tree stopped: its real result
        # still ends that attempt (Forced Program End).
        self._abandoned: RunRequest | None = None

    def _begin_attempt(self, *, restart_series: bool) -> None:
        super()._begin_attempt(restart_series=restart_series)
        self._invoked = False
        self._status = py_trees.common.Status.INVALID
        self._pending_observation = None
        if self._request is not None:
            self._request.withdraw()
        self._request = None
        self._request_effect = False
        self._abandoned = None

    def _send(self, call: Callable[[], Any], *, effect: bool) -> py_trees.common.Status:
        assert self._sender is not None
        self._request = self._sender.submit(call)
        self._request_effect = effect
        return py_trees.common.Status.RUNNING

    def _forget_withdrawn(self) -> None:
        """The request was withdrawn before it was sent: as if never made."""
        if self._request_effect:
            self._invoked = False
        self._request = None
        self._request_effect = False

    def update(self) -> py_trees.common.Status:
        held = self._held_write_result()
        if held is not None:
            return held  # Its output's publication was not answered yet.
        if self._wait_for_admission_release():
            return py_trees.common.Status.RUNNING
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        if self._request is not None and self._request.withdrawn:
            self._forget_withdrawn()
        if self._invoked and self._pending_observation is None and self._request is None:
            return self._status
        try:
            if self._request is not None:
                if not self._request.answered:
                    return py_trees.common.Status.RUNNING
                request, self._request = self._request, None
                result = request.result()
                if result is None:
                    # The read found the outcome still unconfirmed: read again.
                    return self._send(self._pending_observation, effect=False)
            elif self._pending_observation is not None:
                if self._sender is not None:
                    return self._send(self._pending_observation, effect=False)
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
                if self._sender is not None:
                    port, node_id = self._port, self._node_id
                    identity, inputs = self._identity, self._owner_inputs
                    return self._send(
                        lambda: port.invoke(node_id, identity, inputs), effect=True
                    )
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
        # A written output returns the result after the host's answer to it.
        return self._after_write(self._status)

    @property
    def has_active_task(self) -> bool:
        return self._pending_observation is not None or self._request is not None

    def observe_control_state(self) -> str:
        return "active" if self.has_active_task else "settled" if self._invoked else "inactive"

    def cached_control_state(self) -> str:
        return self.observe_control_state()

    def _sent_effect_unanswered(self) -> bool:
        request = self._request
        return (
            request is not None and self._request_effect
            and request.sent and not request.answered
        )

    def reconcile_halt_task(self, *, selected_resume: bool = False) -> str:
        # A selected Resume must not reopen admission while a previously sent
        # transaction is still in flight. Full tree Stop uses the permanent
        # run fence and the owner's child claim; it does not cancel this call.
        if not selected_resume:
            # Halt: a request not sent is withdrawn (the node sends it after
            # Execute); a sent effect keeps the run Halting until its answer,
            # which the node returns on its first tick after Execute.
            if self._request is not None and self._request.withdraw():
                self._forget_withdrawn()
            return "active" if self._sent_effect_unanswered() else "unaffected"
        self.observe_while_operator_stop_held()
        return self.observe_control_state()

    def observe_while_operator_stop_held(self) -> None:
        request = self._request
        if request is not None:
            if self._request_effect and request.withdraw():
                # Not sent before the Stop: it is not sent; the node stays
                # where it is and sends it when it is ticked again.
                self._forget_withdrawn()
                return
            if not request.answered and not request.withdrawn:
                return
        if self._request is not None or self._pending_observation is not None:
            status = self.update()
            if status is not py_trees.common.Status.RUNNING:
                self.stop(status)

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._abandon_write(new_status)  # A written output still ends it.
        request, effect = self._request, self._request_effect
        if request is None:
            return
        self._request = None
        self._request_effect = False
        if request.withdraw():
            if effect:
                self._invoked = False
        elif effect and new_status is py_trees.common.Status.INVALID:
            # Sent: the atomic action completes on its owner (OD-213).
            self._abandoned = request

    def settle_abandoned_request(self) -> bool:
        """Forced Program End: the sent effect of the stopped attempt ends
        it with its real result, its output written as the node writes it.
        True once nothing sent is outstanding (nor a written output's
        publication)."""
        if not super().settle_abandoned_request():
            return False
        request = self._abandoned
        if request is None:
            return True
        if not request.answered:
            return False
        self._abandoned = None
        try:
            result = request.result()
        except (OwnerInvocationHeldError, OwnerInvocationPendingError):
            # Not admitted, or its outcome is unconfirmed: nothing to return.
            return True
        except Exception as exc:
            self._record_owner_error("owner_request_failed", exc)
            self.status = py_trees.common.Status.FAILURE
            return True
        if result.succeeded:
            assert self._identity is not None  # The effect was sent with it.
            status = (
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
            self._status = status
            if self._after_write(status) is py_trees.common.Status.RUNNING:
                # Held until the host answered the output (OD-213 part 3).
                self._abandon_write(py_trees.common.Status.INVALID)
                return False
            self.status = status
        else:
            self._record_owner_failure(result.reason_code, result.message, result.details)
            self.status = py_trees.common.Status.FAILURE
        return True


# The Kernel's gripper command states as `expected_state` names them.
_GRIPPER_STATE_NAMES: Final = {
    "idle": "idle",
    "executing": "moving",
    "completed": "reached",
    "failed": "failed",
}


class GripperStatusWaitAdapter(_OwnerAdapter):
    """Read one command scope at at most 10 Hz using the existing tree ticks.

    Cancelling a read-only waiter has no physical command to cancel. It never
    follows a replacement command/source, renews evidence, or owns a worker.
    A Program Stop, Reset, cancel or a parent ending the wait reports
    `interrupted`. `timeout_s` counts the run's Program time, which stands
    still while the run is halted; with `timeout_while_held: pause` (the
    default) the time an operator Stop holds the wait does not count either,
    so a Stop never turns into a timeout failure when the wait resumes.

    With the run's sender (OD-380 D2) each read leaves the tick: the node
    reads its answer on a later tick. A read not sent yet when the deadline
    passes is withdrawn; a sent one completes and its late answer fails the
    wait as after the deadline. A Halt withdraws a read not sent yet; the
    node sends it again when it is ticked after Execute.
    """

    # The gripper and the state this attempt last read, with the UTC time it
    # was first read: display only.
    wait_observation: Mapping[str, Any] | None = None

    def __init__(
        self,
        *,
        port: OwnerInvocationPort,
        program_clock: ProgramClock,
        operator_stop_state: OperatorMotionStopState | None = None,
        operator_stop_hold_scope: str | None = None,
        run_sender: RunSender | None = None,
        **kwargs: Any,
    ):
        super().__init__(**kwargs)
        self._port = port
        self._sender = run_sender
        self._request: RunRequest | None = None
        self._clock = program_clock
        self._attempt_time = AttemptTime(
            program_clock,
            operator_stop_state=operator_stop_state,
            operator_stop_hold_scope=operator_stop_hold_scope,
        )
        self._next_read = 0.0
        self._scope = None

    def _withdraw_read(self) -> None:
        if self._request is not None:
            self._request.withdraw()  # A sent read completes; nobody reads it.
            self._request = None

    def _begin_attempt(self, *, restart_series: bool) -> None:
        self._withdraw_read()
        super()._begin_attempt(restart_series=restart_series)
        self._scope = None
        self.wait_observation = None
        self._next_read = self._clock.now()
        self._attempt_time.start(
            exclude_held=self._inputs is not None
            and self._inputs.get("timeout_while_held") == "pause"
        )

    def _observe(self, native_state: Any) -> None:
        assert self._owner_inputs is not None  # Checked before every read.
        state = _GRIPPER_STATE_NAMES.get(native_state, native_state)
        previous = self.wait_observation
        if previous is not None and previous.get("state") == state:
            return
        target = self._owner_inputs.parameters.get("target")
        observation: dict[str, Any] = {"kind": "gripper", "observed_at_utc_ns": time.time_ns()}
        if isinstance(target, Mapping) and isinstance(target.get("component_id"), str):
            observation["component_id"] = target["component_id"]
        if isinstance(state, str):
            observation["state"] = state
        self.wait_observation = observation

    def _timed_out(self) -> bool:
        assert self._inputs is not None  # Checked before every deadline test.
        return self._attempt_time.elapsed() >= float(self._inputs["timeout_s"])

    def terminate(self, new_status: py_trees.common.Status) -> None:
        self._withdraw_read()
        self._record_interruption(new_status)

    def update(self) -> py_trees.common.Status:
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        request = self._request
        if request is not None and request.withdrawn:
            request = self._request = None  # Withdrawn by a Halt: read again.
        if self._timed_out() and (request is None or request.withdraw()):
            self._request = None
            self._record_node_failure(
                "gripper_wait_timeout",
                "Expected gripper state was not observed before the deadline.",
            )
            return py_trees.common.Status.FAILURE
        if request is None:
            if self._clock.now() < self._next_read:
                return py_trees.common.Status.RUNNING
            if self._sender is not None:
                port, node_id = self._port, self._node_id
                identity, inputs = self._identity, self._owner_inputs
                self._request = self._sender.submit(
                    lambda: port.invoke(node_id, identity, inputs)
                )
                return py_trees.common.Status.RUNNING
        elif not request.answered:
            return py_trees.common.Status.RUNNING
        try:
            if request is None:
                result = self._port.invoke(self._node_id, self._identity, self._owner_inputs)
            else:
                self._request = None
                result = request.result()
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
        self._observe(result.metadata.get("command_state"))
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
    """An owner refusal's stable `io_*` reason, if it has one."""

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
    source is not fresh the wait stays RUNNING and visibly unavailable.
    The optional `timeout_s` counts the attempt's Program time (it stands
    still during a Halt and, like gripper waits, while an operator Stop
    holds the node); expiry fails the node. The node's end, a Program Stop,
    Reset, cancel or a parent ending it releases the exact watch; a Halt or
    an operator Stop hold keeps it, and a match is reported only when the
    tree ticks again. A transport failure never releases it. A registration
    whose reply is unknown fails the attempt; the port then releases that
    exact attempt's scope without a token.

    With the run's sender (OD-380 D2) the registration, each read and the
    release leave the tick: a register or read is answered on a later tick
    (a tick that reads an answer sends no new read, so the reads keep the
    tick cadence); a release is sent and not awaited, and the next attempt
    registers only once it is sent. A request not sent yet when the timeout
    passes is withdrawn; a sent one completes and its late answer fails the
    wait as after timeout_s. A Halt withdraws a request not sent yet; the
    node sends it again when it is ticked after Execute (a withdrawn release
    leaves the watch to the run's teardown).
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
        run_sender: RunSender | None = None,
        **kwargs: Any,
    ) -> None:
        super().__init__(**kwargs)
        self._port = port
        self._sender = run_sender
        # The registration or read on the run's sender, and the last release.
        self._request: RunRequest | None = None
        self._release: RunRequest | None = None
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
        if self._request is not None:
            # A register or read not sent is never sent. A sent registration's
            # watch stays with the port, which the run's teardown releases.
            self._request.withdraw()
            self._request = None
        task, identity, self._task = self._task, self._identity, None
        if task is None or identity is None:
            return
        port = self._port
        release = port.acknowledge if task.state is OwnerTaskState.SUCCEEDED else port.cancel
        try:
            if self._sender is None:
                release(task.task_id, identity)
            else:
                self._release = self._sender.submit(lambda: release(task.task_id, identity))
        except Exception:
            # Unconfirmed (or the run has ended): the port keeps the exact
            # token and run teardown releases it; the host also releases a
            # retired run's watches.
            pass

    def _release_queued(self) -> bool:
        """The previous attempt's release is not sent yet: the next
        registration waits for it (one queued request per node)."""
        release = self._release
        if release is not None and release.state is RunRequestState.QUEUED:
            return True
        self._release = None
        return False

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

    def _sent(self, call: Callable[[], OwnerTaskSnapshot]) -> OwnerTaskSnapshot | None:
        """The register or read: made inside the tick without a sender;
        otherwise submitted on this tick (None) and its answer taken on a
        later one. What the call raised is raised here."""
        if self._sender is None:
            return call()
        self._request = self._sender.submit(call)
        return None

    def update(self) -> py_trees.common.Status:
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        request = self._request
        if request is not None and request.withdrawn:
            request = self._request = None  # Withdrawn by a Halt: sent again.
        if self._timed_out() and (request is None or request.withdraw()):
            self._request = None
            self._release_watch()
            self._record_node_failure(
                "io_wait_timeout",
                "The expected input level was not observed before timeout_s.",
            )
            return py_trees.common.Status.FAILURE
        if request is not None and not request.answered:
            return py_trees.common.Status.RUNNING
        port, node_id, identity = self._port, self._node_id, self._identity
        try:
            if self._task is None:
                if request is None:
                    gate = self._mock_simulation_ready()
                    if gate is not None:
                        return gate
                    if self._release_queued():
                        return py_trees.common.Status.RUNNING
                    inputs = self._owner_inputs
                    registered = self._sent(lambda: port.start(node_id, identity, inputs))
                else:
                    self._request = None
                    registered = request.result()
                if registered is None:
                    return py_trees.common.Status.RUNNING
                self._task = registered
            else:
                task_id = self._task.task_id
                try:
                    if request is None:
                        observed = self._sent(lambda: port.observe(task_id))
                        if observed is None:
                            return py_trees.common.Status.RUNNING
                    else:
                        self._request = None
                        observed = request.result()
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
            # The register/read crossed the deadline: its result (even a
            # match) arrived after timeout_s and is not accepted.
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

    def reconcile_halt_task(self, *, selected_resume: bool = False) -> str:
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
    "GripperStatusWaitAdapter",
    "PickPlacePlanningAdapter",
]
