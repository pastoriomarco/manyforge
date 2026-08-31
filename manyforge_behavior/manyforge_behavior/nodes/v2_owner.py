"""Thin v2 py_trees leaves over focused headless owner ports."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import py_trees

from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    NodeAttemptSource,
    OwnerInvocationPort,
    OwnerNodeInputs,
    OwnerTaskObservationUnavailableError,
    OwnerTaskPort,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramInterventionPort,
)
from manyforge_behavior.nodes.stop_policy import OperatorMotionStopState
from manyforge_behavior.orchestration_values import (
    NodeAttemptInputResolver,
    ProgramValueError,
)


class _OwnerAdapter(py_trees.behaviour.Behaviour):
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
    ) -> None:
        super().__init__(name=name)
        self._node_id = node_id
        self._node_instance_id = node_instance_id
        self._attempt_source = attempt_source
        self._parameters = dict(parameters)
        self._value_sources = {
            key: dict(source) for key, source in value_sources.items()
        }
        self._input_resolver = input_resolver
        self._output_key_parameter = output_key_parameter
        self._blackboard_read_parameters = blackboard_read_parameters
        self._inputs: Mapping[str, Any] | None = None
        self._owner_inputs: OwnerNodeInputs | None = None
        self._identity: NodeAttemptIdentity | None = None
        self._input_error: str | None = None
        self._failure_reason_code: str | None = None
        self._failure_message = ""

    def initialise(self) -> None:
        self._inputs = None
        self._owner_inputs = None
        self._identity = None
        self._input_error = None
        self._failure_reason_code = None
        self._failure_message = ""
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
                embedded_values=snapshot.embedded_values,
            )
        except (KeyError, ProgramValueError, TypeError, ValueError) as exc:
            self._input_error = str(exc)
            self._record_failure("node_input_unavailable", str(exc))

    @property
    def failure_reason_code(self) -> str | None:
        return self._failure_reason_code

    @property
    def failure_message(self) -> str:
        return self._failure_message

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
            self._record_failure("owner_result_invalid", "output is absent")
            return False
        reserved_metadata = {
            "node_id": self._node_id,
            "node_instance_id": self._node_instance_id,
            **adapter_metadata,
        }
        collisions = set(owner_metadata) & set(reserved_metadata)
        if collisions:
            self._record_failure(
                "owner_result_invalid",
                "owner metadata replaces reserved fields "
                f"{sorted(collisions)}",
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
            self._record_failure("owner_output_rejected", str(exc))
            return False
        return True

    def _record_failure(self, reason_code: str | None, message: str) -> None:
        reason = reason_code or "owner_result_invalid"
        self._failure_reason_code = reason
        self._failure_message = message
        self.feedback_message = f"{reason}: {message}".rstrip()


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
            "owner_assessment": (
                assessment if isinstance(assessment, Mapping) else None
            ),
        }

    def _settle(self, task: OwnerTaskSnapshot) -> py_trees.common.Status:
        assessment = task.metadata.get("recovery_assessment")
        operator_stop = (
            task.state is OwnerTaskState.INTERRUPTED
            and isinstance(assessment, Mapping)
            and assessment.get("recovery_class") == "ordinary_operator_stop"
        )
        if operator_stop and self._operator_stop_state is not None:
            if not self._operator_stop_state.hold(
                task.task_id, dict(task.metadata)
            ):
                self._record_failure(
                    "owner_stop_identity_missing",
                    "ordinary operator Stop omitted its exact Stop identity",
                )
                return py_trees.common.Status.FAILURE
        if self._intervention_reported:
            if self._intervention_port is None or self._identity is None:
                self._record_failure(
                    "program_intervention_port_unavailable",
                    "held recovery lost its exact owner identity",
                )
                return py_trees.common.Status.FAILURE
            try:
                resolution = self._intervention_port.consume_resolution(
                    self._identity
                )
            except Exception as exc:
                self._record_failure(
                    "program_recovery_resolution_failed", str(exc)
                )
                return py_trees.common.Status.FAILURE
            if resolution is None:
                return py_trees.common.Status.RUNNING
            action, result = resolution
            if action == "retry":
                # A fresh attempt takes a fresh identity and input snapshot;
                # the already-submitted owner request is never changed.
                if operator_stop and self._operator_stop_state is not None:
                    self._operator_stop_state.resolve(task.task_id)
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
            self._record_failure(
                "manual_recovery_failure", "operator staged a failed result"
            )
            return py_trees.common.Status.FAILURE
        if task.state is OwnerTaskState.ACTIVE:
            return py_trees.common.Status.RUNNING
        if task.state is OwnerTaskState.SUCCEEDED:
            if not self._acknowledged:
                if self._identity is None:
                    self._record_failure(
                        "owner_result_invalid",
                        "node-attempt identity is absent",
                    )
                    return py_trees.common.Status.FAILURE
                try:
                    self._port.acknowledge(task.task_id, self._identity)
                except Exception as exc:
                    self._record_failure("owner_acknowledgement_failed", str(exc))
                    return py_trees.common.Status.FAILURE
                self._acknowledged = True
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
                self._record_failure(
                    "owner_result_invalid",
                    "owner returned an unknown authored failure policy",
                )
                return py_trees.common.Status.FAILURE
            if policy == "pause_for_operator":
                if self._intervention_port is None or self._identity is None:
                    self._record_failure(
                        "program_intervention_port_unavailable",
                        "pause_for_operator requires the production intervention port",
                    )
                    return py_trees.common.Status.FAILURE
                parameter = task.metadata.get(
                    "authored_failure_policy_parameter"
                )
                if not isinstance(parameter, str) or not parameter:
                    self._record_failure(
                        "owner_result_invalid",
                        "pause_for_operator has no authored policy parameter",
                    )
                    return py_trees.common.Status.FAILURE
                if not self._intervention_reported:
                    try:
                        current = self._intervention_port.read(
                            self._identity.program_run_id
                        )
                        if current is not None and current.identity != self._identity:
                            # Parallel stopped attempts are recovered one at a
                            # time through the existing exact-attempt surface.
                            return py_trees.common.Status.RUNNING
                        self._intervention_port.require(
                            self._identity,
                            reason_code=(
                                task.reason_code or "operator_intervention_required"
                            ),
                            message=task.message,
                            policy_parameter=parameter,
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
                        self._record_failure(
                            "program_intervention_record_failed",
                            str(exc),
                        )
                        return py_trees.common.Status.FAILURE
                    self._intervention_reported = True
                self._record_failure(task.reason_code, task.message)
                # Keep this exact terminal owner task in place.  The production
                # scheduler removes the run from automatic ticking, so neither
                # a retry nor a stale output write can occur.
                return py_trees.common.Status.RUNNING
        self._record_failure(task.reason_code, task.message)
        return py_trees.common.Status.FAILURE

    def update(self) -> py_trees.common.Status:
        if (
            self._input_error is not None
            or self._identity is None
            or self._owner_inputs is None
        ):
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
                    self._record_failure(
                        "owner_task_identity_changed",
                        "owner observation selected another task",
                    )
                    return py_trees.common.Status.FAILURE
                self._task = observed
        except Exception as exc:
            self._record_failure("owner_request_failed", str(exc))
            return py_trees.common.Status.FAILURE
        return self._settle(self._task)

    def observe_while_operator_stop_held(self) -> None:
        """Refresh only an already accepted task while its subtree is held."""

        if self._task is None or self._identity is None:
            return
        try:
            if self._task.state is OwnerTaskState.ACTIVE:
                observed = self._port.observe(self._task.task_id)
                if observed.task_id != self._task.task_id:
                    self._record_failure(
                        "owner_task_identity_changed",
                        "owner observation selected another task",
                    )
                    return
                self._task = observed
            self._settle(self._task)
        except OwnerTaskObservationUnavailableError:
            return
        except Exception as exc:
            self._record_failure("owner_request_failed", str(exc))

    def reconcile_full_stop_task(self) -> str:
        """Observe accepted Program-scope work without ticking the tree.

        Only the task families captured by ``ProgramRunScopePort`` take part.
        Unrelated asynchronous owners remain preserved and are not advanced by
        Full Stop settlement.
        """

        if self._owner_port_name not in {"kernel_operations", "pick_place"}:
            return "unaffected"
        if self._task is None:
            return "inactive"
        self.observe_while_operator_stop_held()
        task = self._task
        if task is None:
            return "inactive"
        if task.state is OwnerTaskState.ACTIVE:
            return "active"
        assessment = task.metadata.get("recovery_assessment")
        if (
            task.state is OwnerTaskState.INTERRUPTED
            and isinstance(assessment, Mapping)
            and assessment.get("recovery_class") == "ordinary_operator_stop"
        ):
            return "held"
        if task.state is OwnerTaskState.SUCCEEDED:
            return "settled"
        return "unrecoverable"

    def terminate(self, new_status: py_trees.common.Status) -> None:
        if (
            new_status is py_trees.common.Status.INVALID
            and self._task is not None
            and self._task.state is OwnerTaskState.ACTIVE
            and self._identity is not None
        ):
            try:
                self._port.cancel(self._task.task_id, self._identity)
            except Exception as exc:
                self._record_failure("owner_cancellation_failed", str(exc))


class OwnerInvocationAdapter(_OwnerAdapter):
    """Issue one bounded synchronous owner request per node attempt."""

    def __init__(self, *, port: OwnerInvocationPort, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self._port = port
        self._invoked = False
        self._status = py_trees.common.Status.INVALID

    def initialise(self) -> None:
        super().initialise()
        self._invoked = False
        self._status = py_trees.common.Status.INVALID

    def update(self) -> py_trees.common.Status:
        if (
            self._input_error is not None
            or self._identity is None
            or self._owner_inputs is None
        ):
            return py_trees.common.Status.FAILURE
        if self._invoked:
            return self._status
        self._invoked = True
        try:
            result = self._port.invoke(
                self._node_id,
                self._identity,
                self._owner_inputs,
            )
        except Exception as exc:
            self._record_failure("owner_request_failed", str(exc))
            self._status = py_trees.common.Status.FAILURE
            return self._status
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
            self._record_failure(result.reason_code, result.message)
            self._status = py_trees.common.Status.FAILURE
        return self._status


__all__ = ["OwnerInvocationAdapter", "OwnerTaskAdapter"]
