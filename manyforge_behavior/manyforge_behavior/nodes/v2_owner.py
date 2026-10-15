"""Thin v2 py_trees leaves over focused headless owner ports."""

from __future__ import annotations

from collections.abc import Mapping
import math
from typing import Any

import py_trees

from manyforge_behavior.failure_details import (
    NodeFailureOrigin,
    exception_failure_details,
)
from manyforge_behavior.orchestration_ports import (
    PROGRAM_MOTION_STOP_RECOVERY_CLASSES,
    NodeAttemptIdentity,
    NodeAttemptSource,
    OwnerInvocationPort,
    OwnerInvocationPendingError,
    OwnerInvocationHeldError,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskPort,
    OwnerTaskSettlementPendingError,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramInterventionPort,
    describe_owner_failure,
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

    def initialise(self) -> None:
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

    def _wait_for_admission_release(self) -> bool:
        if self._held_before_admission is None:
            return False
        state = self._operator_stop_state
        scope = None if state is None else state.snapshot()
        if (scope is None or state.observation_error() is not None
            or scope.selection_revision <= self._held_before_admission
            or self._node_instance_id in scope.held_nodes):
            return True
        self.initialise()  # Proven-unstarted work, fresh attempt after owner release.
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


class OwnerTaskAdapter(_OwnerAdapter):
    """Submit once, observe owner state, and acknowledge only exact success."""

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
        self._acknowledged = False
        self._intervention_reported = False

    def initialise(self) -> None:
        super().initialise()
        self._task = None
        self._acknowledged = False
        self._intervention_reported = False

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

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        assessment = task.metadata.get("recovery_assessment")
        operator_stop = (
            task.state is OwnerTaskState.INTERRUPTED
            and isinstance(assessment, Mapping)
            and assessment.get("recovery_class") in PROGRAM_MOTION_STOP_RECOVERY_CLASSES
        )
        self._held_after_operator_stop = (
            operator_stop and assessment.get("recovery_class") == "ordinary_operator_stop"
        )
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
            if not self._intervention_reported:
                self._operator_stop_state.refresh()
        if self._intervention_reported:
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
                # A fresh attempt takes a fresh identity and input snapshot;
                # the already-submitted owner request is never changed.
                self.initialise()
                return py_trees.common.Status.RUNNING
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
            if not self._acknowledged:
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
                self._acknowledged = True
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
                if not self._intervention_reported:
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
                    self._intervention_reported = True
                    if self._operator_stop_state is not None:
                        self._operator_stop_state.note_task_observation_change()
                self._record_owner_failure(task.reason_code, task.message, task.details)
                # Keep this exact terminal owner task in place.  The production
                # scheduler removes the run from automatic ticking, so neither
                # a retry nor a stale output write can occur.
                return py_trees.common.Status.RUNNING
        self._record_owner_failure(task.reason_code, task.message, task.details)
        return py_trees.common.Status.FAILURE

    def update(self) -> py_trees.common.Status:
        if self._wait_for_admission_release():
            return py_trees.common.Status.RUNNING
        if self._input_error is not None or self._identity is None or self._owner_inputs is None:
            return py_trees.common.Status.FAILURE
        try:
            if self._task is None:
                self._task = self._port.start(
                    self._node_id,
                    self._identity,
                    self._owner_inputs,
                )
                if self._require_initial_running:
                    return py_trees.common.Status.RUNNING
            elif self._task.state is OwnerTaskState.ACTIVE:
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
            self._hold_before_admission(error)
            return py_trees.common.Status.RUNNING
        except Exception as exc:
            self._record_owner_error("owner_request_failed", exc)
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
            status = self._settle(self._task)
            if status is not py_trees.common.Status.RUNNING:
                self.stop(status)
        except OwnerTaskObservationUnavailableError:
            return
        except OwnerInvocationHeldError as error:
            self._task = None
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
        if self._task is None:
            return "inactive"
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
                self._hold_before_admission(error)
                return "inactive"
            if observed.task_id != task.task_id:
                raise ValueError("owner observation selected another task")
            task = observed
        return self._control_state(task)

    def cached_control_state(self) -> str:
        """Project the last retained owner fact without performing owner I/O."""

        return self._control_state(self._task)

    def _control_state(self, task: OwnerTaskSnapshot | None) -> str:
        if task is None:
            return "inactive"
        if task.state is OwnerTaskState.ACTIVE:
            return "active"
        if (task.state is OwnerTaskState.SUCCEEDED or task.settlement_proven) and not self._acknowledged:
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

    def terminate(self, new_status: py_trees.common.Status) -> None:
        # Halt, Stop or cancel of a running attempt: a cancellable node
        # reports `interrupted`, including a held or not yet admitted one,
        # once the stop is confirmed.
        self._record_interruption(new_status)
        if (
            new_status is py_trees.common.Status.INVALID
            and self._task is not None
            and self._task.state is OwnerTaskState.ACTIVE
            and self._identity is not None
        ):
            try:
                self._port.cancel(self._task.task_id, self._identity)
            except Exception as exc:
                # The owner refused the cancellation: the stop is not
                # confirmed, so the attempt reports `failure`, never
                # `interrupted`, and the run pauses for operator attention.
                self._record_stop_failure()
                self._record_owner_error("owner_cancellation_failed", exc)


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

    def initialise(self) -> None:
        super().initialise()
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

    def initialise(self) -> None:
        super().initialise()
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

    def initialise(self) -> None:
        super().initialise()
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


__all__ = ["OwnerInvocationAdapter", "OwnerTaskAdapter", "GripperCommandAdapter", "GripperStatusWaitAdapter"]
