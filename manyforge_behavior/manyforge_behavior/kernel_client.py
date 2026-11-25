"""Thin Behavior client for the resource-addressed native execution owner."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, Mapping
import unicodedata


# Transport loss and malformed replies (the core's GatewayError codes): they
# cannot prove what the owner did, so they are never an owner refusal (or a
# teardown).
EXECUTION_TRANSPORT_FAILURE_CODES = frozenset(
    {
        "execution_owner_unavailable",
        "execution_command_transport_failure",
        "execution_command_response_invalid",
    }
)


class ExecutionCommandError(RuntimeError):
    """The execution owner's typed refusal of one command (an ``ok: false``
    reply), or a refusal reply that names no code. Transport and envelope
    failures are the core's ``GatewayError`` (codes in
    ``EXECUTION_TRANSPORT_FAILURE_CODES``)."""

    def __init__(
        self,
        code: str,
        message: str,
        details: Mapping[str, Any] | None = None,
        producer: str = "kernel",
    ) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = dict(details or {})
        self.producer = producer


def _refusal(reply: Mapping[str, Any]) -> ExecutionCommandError:
    error = reply.get("error")
    if (
        isinstance(error, Mapping)
        and isinstance(error.get("code"), str)
        and error["code"]
        and isinstance(error.get("message"), str)
        and error["message"]
        and isinstance(error.get("details", {}), Mapping)
    ):
        producer = error.get("producer")
        return ExecutionCommandError(
            error["code"], error["message"], error.get("details"),
            producer if isinstance(producer, str) and producer else "kernel",
        )
    return ExecutionCommandError(
        "execution_command_response_invalid",
        "native execution owner returned an invalid error response",
        producer="headless_api",
    )


# Refusals of a Program selection release, Halt release or node motion
# attention check that precede any release (a lost or malformed reply stays
# outcome-unknown).
_SELECTION_RELEASE_REFUSALS = frozenset(
    {
        "program_halt_release_stale",
        "program_selection_manual_operation_active",
        "program_selection_stale",
        "program_selection_unknown",
        "program_node_unknown",
        "program_selection_revision_stale",
        "program_selection_not_ready",
        "program_selection_not_releasable",
        "program_selection_recovery_required",
        "program_children_unsettled",
        "program_run_admission_in_flight",
        "operator_alarm_interlock_active",
        "operator_alarm_state_unavailable",
    }
)


# The wire form of an operation as the host decodes it: optional members
# carried as null when unset, and those omitted when null. A motion's
# `planner` (and each of its members) and `runtime_object_bindings` are sent
# as built: the host reads an absent one as null or empty.
_POSE_GOAL_OMITTED = (
    "motion_type",
    "max_translation_step_m",
    "max_rotation_step_rad",
    "max_angular_speed_rad_s",
    "max_linear_acceleration_mps2",
    "max_angular_acceleration_rad_s2",
    "speed_mode",
)
_COLLISION_RULE_OMITTED = ("a", "b", "clearance_m", "contact", "release")
_CONTACT_OMITTED = (
    "max_penetration_m", "end_from_fraction", "max_compression_step_m", "spring_group",
)
_ACTION_SPAN_NULLABLE = (
    "program_run_id", "node_instance_id", "node_attempt_id", "parent_node_attempt_id",
)
_JOINT_LIMIT_NULLABLE = ("max_velocity_rad_s", "max_acceleration_rad_s2", "max_jerk_rad_s3")


def _without_null(value: Mapping[str, Any], names: tuple[str, ...]) -> dict[str, Any]:
    return {key: item for key, item in value.items() if not (key in names and item is None)}


def _with_null(value: Mapping[str, Any], names: tuple[str, ...]) -> dict[str, Any]:
    return {**{name: None for name in names}, **value}


def _context_wire(context: Any) -> Any:
    if not isinstance(context, Mapping):
        return context
    return {"request_key": None, **context}


def _selection_wire(selection: Any) -> Any:
    if isinstance(selection, Mapping) and selection.get("kind") == "attached_object":
        return _without_null(selection, ("collision_body_role_id",))
    return selection


def _collision_rule_wire(rule: Any) -> Any:
    if not isinstance(rule, Mapping):
        return rule
    rule = _without_null(rule, _COLLISION_RULE_OMITTED)
    for side in ("a", "b"):
        if isinstance(rule.get(side), list):
            rule[side] = [_selection_wire(selection) for selection in rule[side]]
    if isinstance(rule.get("contact"), Mapping):
        rule["contact"] = _without_null(rule["contact"], _CONTACT_OMITTED)
    return rule


def _motion_payload_wire(payload: Mapping[str, Any]) -> dict[str, Any]:
    payload = {"target_reference": None, "offset_reference": None, **payload}
    if "goal_frame_reference" in payload and payload["goal_frame_reference"] is None:
        del payload["goal_frame_reference"]
    if isinstance(payload.get("speed_profile"), Mapping):
        payload["speed_profile"] = _with_null(
            payload["speed_profile"], ("max_tcp_linear_speed_mps",)
        )
    limits = payload.get("per_joint_dynamics_limits")
    if isinstance(limits, Mapping):
        payload["per_joint_dynamics_limits"] = {
            joint: _with_null(limit, _JOINT_LIMIT_NULLABLE) if isinstance(limit, Mapping) else limit
            for joint, limit in limits.items()
        }
    if isinstance(payload.get("collision_rules"), list):
        payload["collision_rules"] = [
            _collision_rule_wire(rule) for rule in payload["collision_rules"]
        ]
    motion = payload.get("motion")
    if isinstance(motion, Mapping):
        if motion.get("kind") == "pose_goal":
            payload["motion"] = _without_null(motion, _POSE_GOAL_OMITTED)
        elif motion.get("kind") == "cartesian_path":
            payload["motion"] = _without_null(motion, ("speed_mode",))
    return payload


def _participant_wire(participant: Any) -> Any:
    if not isinstance(participant, Mapping):
        return participant
    participant = dict(participant)
    if isinstance(participant.get("action_span"), Mapping):
        participant["action_span"] = _with_null(
            participant["action_span"], _ACTION_SPAN_NULLABLE
        )
    payload = participant.get("payload")
    if isinstance(payload, Mapping) and payload.get("kind") == "motion":
        participant["payload"] = _motion_payload_wire(payload)
    return participant


def _operation_wire(operation: Mapping[str, Any]) -> dict[str, Any]:
    """One operation intent in its wire form: its unset optional members as
    the host decodes them (a joint limit row always carries its three caps;
    a pose goal omits its unset limits). Everything else is sent as given."""

    operation = {"program_run_id": None, **operation}
    operation.pop("application_start_id", None)
    if "manual_handover" in operation and operation["manual_handover"] is None:
        del operation["manual_handover"]
    if "request_context" in operation:
        operation["request_context"] = _context_wire(operation["request_context"])
    if isinstance(operation.get("participants"), list):
        operation["participants"] = [
            _participant_wire(participant) for participant in operation["participants"]
        ]
    return operation


def _planning_intent_wire(intent: Mapping[str, Any]) -> dict[str, Any]:
    intent = dict(intent)
    for phase in ("approach_operation", "descend_operation", "tool_operation", "retreat_operation"):
        if isinstance(intent.get(phase), Mapping):
            intent[phase] = _operation_wire(intent[phase])
    return intent


def _control_wire(request: Mapping[str, Any]) -> dict[str, Any]:
    request = {"reason": None, **request}
    request.pop("application_start_id", None)
    if "request_context" in request:
        request["request_context"] = _context_wire(request["request_context"])
    return request


_MAX_STOP_REASON_BYTES = 1024


def _sparse_stop_wire(request: Mapping[str, Any]) -> dict[str, Any]:
    """A Stop selector: unused selector kinds and an absent reason omitted;
    the request context is ``{request_key, source_label}``."""

    request = {
        key: value
        for key, value in request.items()
        if value is not None and key != "application_start_id"
    }
    reason = request.get("reason")
    if "reason" in request and not (
        isinstance(reason, str)
        and reason
        and unicodedata.normalize("NFC", reason) == reason
        and len(reason.encode("utf-8")) <= _MAX_STOP_REASON_BYTES
    ):
        # An unusable Stop reason is dropped, never a refusal.
        del request["reason"]
    context = request.get("request_context")
    if isinstance(context, Mapping):
        request["request_context"] = {
            "request_key": context.get("request_key"),
            "source_label": context.get("source_label"),
        }
    return request


class KernelClientResponseError(RuntimeError):
    """Raised when a transport returns a successful but malformed projection."""


class KernelStopNotResidentError(RuntimeError):
    """``read_stop``: the event's record was overwritten in the Kernel's ring
    (refusal ``stop_status_not_resident``)."""

    code = "stop_status_not_resident"

    def __init__(self, event_id: int) -> None:
        super().__init__(f"Stop event {event_id} is no longer resident")
        self.event_id = event_id


class KernelOperationAdmissionRejectedError(RuntimeError):
    """The execution owner explicitly rejected an operation before acceptance."""

    def __init__(
        self,
        code: str,
        message: str,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = dict(details or {})


class KernelImmediateStepFailedError(RuntimeError):
    """A nonresident immediate step failed after the exact admission."""

    def __init__(self, code: str, message: str, details: Mapping[str, Any],
                 active_tcp: object) -> None:
        super().__init__(message)
        self.code = code
        self.details = dict(details)
        self.active_tcp = active_tcp


class IoOutputWriteFailedError(RuntimeError):
    """An output write ended `failed` or `unknown`, with the
    host's typed error. It is never resent."""

    def __init__(self, code: str, message: str, outcome: str) -> None:
        super().__init__(message)
        self.code = code
        self.outcome = outcome


class KernelOperationObservationUnavailableError(RuntimeError):
    """An exact operation read could not reach the native owner temporarily."""


class KernelPlanningJobRefusedError(RuntimeError):
    """The execution host refused a pick/place planning-job command with its
    typed code (for example ``planning_job_exists``, ``planning_job_unknown``,
    ``execution_planning_jobs_busy``)."""

    def __init__(
        self, code: str, message: str, details: Mapping[str, Any] | None = None
    ) -> None:
        super().__init__(message)
        self.code = code
        self.details = dict(details or {})


PLANNING_JOB_FINAL_STATES = frozenset({"done", "failed", "cancelled"})
_PLANNING_JOB_STATES = frozenset({"accepted", "planning"}) | PLANNING_JOB_FINAL_STATES


class KernelOperationNotYetPushedError(KernelOperationObservationUnavailableError):
    """The pushed operations table is older than the operation's acceptance."""


_MAX_SAFE = 9_007_199_254_740_991

# The core's pushed copy (``manyforge_headless_py.ExecutionEvents``) raises
# its typed GatewayError with these codes.
_PUSHED_UNAVAILABLE = "execution_event_mirror_unavailable"
_PUSHED_BEHIND = "execution_event_mirror_behind"


def _counter(value: object) -> int | None:
    return value if type(value) is int and 0 <= value <= _MAX_SAFE else None


def _pushed_read(read: Callable[[], Any]) -> Any:
    """One read of the core's pushed copy, its typed refusals mapped to the
    observation errors the nodes handle."""

    try:
        return read()
    except Exception as error:
        code = getattr(error, "code", None)
        message = str(getattr(error, "message", error))
        if code == _PUSHED_BEHIND:
            raise KernelOperationNotYetPushedError(message) from error
        if code == _PUSHED_UNAVAILABLE:
            raise KernelOperationObservationUnavailableError(message) from error
        raise


class KernelOperationSettlementPendingError(RuntimeError):
    """A terminal operation still has one accepted subordinate effect in flight."""


class LegacyKernelMockUnavailableError(RuntimeError):
    """The removed stateful Python mock cannot become a second Kernel owner."""


class ExecutionState(Enum):
    """Closed vocabulary projected by the C++ addressed execution owner."""

    QUEUED = "queued"
    RESOLVING = "resolving"
    PLANNING = "planning"
    VALIDATING = "validating"
    PREPARED = "prepared"
    DISPATCHING = "dispatching"
    EXECUTING = "executing"
    STOPPING = "stopping"
    COMPLETED = "completed"
    STOPPED = "stopped"
    FAILED = "failed"
    INTERRUPTED = "interrupted"
    INTERVENTION_REQUIRED = "intervention_required"
    CANCELLED_PRE_DISPATCH = "cancelled_pre_dispatch"


_ACTION_SOURCES = frozenset(
    {"program_node", "direct_control", "robot_control", "mcp", "system"}
)


@dataclass(frozen=True, slots=True)
class ActionSpan:
    """Transport-neutral behavior-owned action correlation."""

    action_source: str = "system"
    action_span_id: str = ""
    program_run_id: int | None = None
    node_instance_id: str | None = None
    # A node attempt number; a pick/place phase child span keeps its
    # composed "<attempt>:<phase>" text (see the slice notes).
    node_attempt_id: int | str | None = None
    parent_node_attempt_id: int | None = None

    def __post_init__(self) -> None:
        if self.action_source not in _ACTION_SOURCES:
            raise ValueError("action_source is unsupported")
        if not self.action_span_id or len(self.action_span_id.encode("utf-8")) > 256:
            raise ValueError("action_span_id is invalid")
        if self.action_source == "program_node":
            if not self.node_instance_id or not self.node_attempt_id:
                raise ValueError(
                    "program_node spans require node_instance_id and node_attempt_id"
                )
            if self.action_span_id != str(self.node_attempt_id):
                raise ValueError(
                    "program_node action_span_id must equal node_attempt_id"
                )

    def as_request(self) -> dict[str, Any]:
        # The host's action span carries attempt ids as text (P-21 (a)).
        return {
            "action_source": self.action_source,
            "action_span_id": self.action_span_id,
            "program_run_id": self.program_run_id,
            "node_instance_id": self.node_instance_id,
            "node_attempt_id": (
                None if self.node_attempt_id is None else str(self.node_attempt_id)
            ),
            "parent_node_attempt_id": (
                None
                if self.parent_node_attempt_id is None
                else str(self.parent_node_attempt_id)
            ),
        }


class KernelClient:
    """Stateless adapter over the core's gateway; the C++ host remains the
    only execution owner.

    ``gateway`` is the process's one ``manyforge_headless_py.Gateway``: one
    method per host command, named as the wire command, taking the request
    members and returning the host's reply (``ok`` true or false). Its
    ``GatewayError`` (transport and envelope failures) propagates unchanged.
    """

    def __init__(
        self,
        gateway: Any,
        events: Any = None,
    ) -> None:
        for command in ("identity", "submit", "read", "reset", "acknowledge_completed", "stop"):
            if not callable(getattr(gateway, command, None)):
                raise TypeError(f"execution gateway has no callable {command}")
        self._gateway = gateway
        # The core's event follower (``manyforge_headless_py.ExecutionEvents``)
        # and its pushed operations and planning jobs (OD-213); None in a
        # process without the event subscription, which reads instead.
        self._events = events

    @property
    def pushes_operations(self) -> bool:
        """True when ``observed_operation`` reads the pushed table."""

        return self._events is not None

    def _send(self, command: str, members: Mapping[str, Any] | None = None) -> dict[str, Any]:
        """The host's reply to one command, whatever its ``ok``."""

        method = getattr(self._gateway, command, None)
        if not callable(method):
            raise TypeError(f"execution gateway has no callable {command}")
        return method(dict(members or {}))

    def _owner(self, command: str, members: Mapping[str, Any] | None = None) -> dict[str, Any]:
        """The host's success reply; a refusal raises ``ExecutionCommandError``."""

        reply = self._send(command, members)
        if not isinstance(reply, dict) or reply.get("ok") is not True:
            raise _refusal(reply if isinstance(reply, dict) else {})
        return reply

    def _selection_owner(self, command: str, members: Mapping[str, Any]) -> dict[str, Any]:
        """A selection or Halt release: a refusal that precedes the release is
        ``ProgramSelectionReleaseRejectedError``."""

        try:
            return self._owner(command, members)
        except ExecutionCommandError as error:
            if error.details.get("reason") in _SELECTION_RELEASE_REFUSALS:
                from manyforge_behavior.orchestration_ports import (
                    ProgramSelectionReleaseRejectedError,
                )

                raise ProgramSelectionReleaseRejectedError(
                    str(error), code=error.code, details=error.details
                ) from error
            raise

    @staticmethod
    def _successful(value: object, *, label: str) -> dict[str, Any]:
        if not isinstance(value, dict) or value.get("ok") is not True:
            raise KernelClientResponseError(
                f"execution owner {label} did not return a successful envelope"
            )
        return value

    def identity(self) -> dict[str, Any]:
        return self._owner("identity")

    def register_program_run_capture(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Install a claim-bound capture: only identities are sent; the
        native product boundary supplies the table."""

        return self._owner("register_program_run_capture", {"capture": dict(body)})

    def targets(self) -> dict[str, Any]:
        return self._owner("targets")

    def read_active_tcp(self, *, ordered: bool = False) -> dict[str, Any]:
        if type(ordered) is not bool:
            raise TypeError("ordered must be boolean")
        return self._owner("read_active_tcp", {"ordered": True} if ordered else {})

    def reset_active_tcp(self) -> dict[str, Any]:
        return self._owner("reset_active_tcp")

    def resolve_attach_active_tcp(
        self, binding_id: str, target: Mapping[str, str]
    ) -> dict[str, str]:
        """The Active TCP holder frame of one attach target."""

        frame = self._owner(
            "resolve_attach_active_tcp",
            {"program_binding_id": binding_id, "target": dict(target)},
        ).get("frame")
        if not isinstance(frame, Mapping):
            raise ExecutionCommandError(
                "execution_command_response_invalid",
                "Invalid Active TCP holder response",
                producer="headless_api",
            )
        return dict(frame)

    def control_state(self) -> dict[str, Any]:
        return self._owner("control_state")

    def observation(self) -> dict[str, Any]:
        return self._owner("observation")

    def query_target(self, body: Mapping[str, Any]) -> dict[str, Any]:
        # The query names only the members its kind uses.
        return self._owner(
            "query_target",
            {"query": {key: value for key, value in body.items() if value is not None}},
        )

    def register_io_watch(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """Register one zero-claim level watch: ``{registration, result}``."""

        reply = self._owner("register_io_watch", {"request": dict(request)})
        response = {"registration": reply.get("registration"), "result": reply.get("result")}
        if not isinstance(response["registration"], dict) or not isinstance(
            response["result"], dict
        ):
            raise KernelClientResponseError("I/O watch registration is malformed")
        return response

    def read_io_watch(self, registration: Mapping[str, Any]) -> dict[str, Any]:
        """The watch's current ``{state, observation, matched_sample}``."""

        result = self._owner("read_io_watch", {"registration": dict(registration)}).get("result")
        if not isinstance(result, dict) or result.get("state") not in {
            "pending", "matched", "released"
        }:
            raise KernelClientResponseError("I/O watch read is malformed")
        return result

    def release_io_watch(self, registration: Mapping[str, Any]) -> None:
        """Exact-token idempotent release; a released token stays released."""

        self._owner("release_io_watch", {"registration": dict(registration)})

    def release_io_watch_scope(self, scope_request: Mapping[str, Any]) -> None:
        """Release one exact node attempt's watch without its token: the host
        answers after fencing the attempt and releasing any watch it holds."""

        self._owner("release_io_watch", {"scope_request": dict(scope_request)})

    def start_mock_io_node_simulation(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """The host's exact accepted acknowledgement (a resent request is
        answered with the retained original result)."""

        ack = self._owner("start_mock_io_node_simulation", {"request": dict(request)})
        if ack.get("status") != "accepted" or not isinstance(ack.get("signals"), list):
            raise KernelClientResponseError("Mock simulation acknowledgement is malformed")
        return ack

    def end_mock_io_node_simulation(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """The host's reply to one node attempt's end (idempotent)."""

        return self._owner("end_mock_io_node_simulation", {"request": dict(request)})

    def set_mock_io_run_paused(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """The host's reply to the Run's Halt state."""

        return self._owner("set_mock_io_run_paused", {"request": dict(request)})

    def publish_program_bbkeys(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """One ordered BB key frame; only a success reply ACKs it.

        ``accepted`` names this frame's sequence; ``superseded`` (its
        occurrence was retired by a Reset, a new Run or an Apply) names none.
        A refusal keeps the owner's code (for example ``bb_source_not_ready``).
        """

        ack = self._owner("publish_program_bbkeys", {"request": dict(request)})
        status = ack.get("status")
        if status not in {"accepted", "superseded"} or (status == "accepted") != (
            ack.get("accepted_sequence") is not None
        ):
            raise ExecutionCommandError(
                "execution_command_response_invalid",
                "BB key publication reply is invalid",
                producer="headless_api",
            )
        return ack

    def read_interrupt_rule_state_events(self, after_sequence: int) -> dict[str, Any]:
        """One page of the ``events`` command, returned whole for the global
        rule state mirror (it reads ``interrupt_rule_catalog`` and
        ``interrupt_rule_state_baseline``)."""

        return self._owner("events", {"after_sequence": after_sequence, "limit": 16})

    def reset_robot_position_capability(self) -> dict[str, Any]:
        return self._owner("reset_robot_position_capability")

    def reset_robot_position(self, request: Mapping[str, Any]) -> None:
        self._owner("reset_robot_position", dict(request))

    def execute(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        def refusal(code: str, message: str, details: object, active_tcp: object) -> None:
            detail = dict(details) if isinstance(details, Mapping) else {}
            participants = intent.get("participants")
            is_set_tcp = (
                isinstance(participants, (list, tuple)) and len(participants) == 1
                and isinstance(participants[0], Mapping)
                and isinstance(participants[0].get("payload"), Mapping)
                and participants[0]["payload"].get("kind") == "set_tcp"
            )
            if is_set_tcp and detail.get("immediate_step") == "admitted":
                context = intent.get("request_context")
                expected = context.get("request_key") if isinstance(context, Mapping) else None
                if (not isinstance(expected, str) or not expected
                    or detail.get("request_key") != expected
                    or type(detail.get("operation_id")) is not int
                    or detail["operation_id"] < 1):
                    raise KernelClientResponseError("immediate-step failure receipt is invalid")
                raise KernelImmediateStepFailedError(code, message, detail, active_tcp)
            raise KernelOperationAdmissionRejectedError(code, message, detail)

        try:
            response = self._send("submit", {"operation": _operation_wire(intent)})
        except Exception as exc:
            # A refused local precondition of the core keeps its typed code
            # as an admission refusal; transport loss cannot prove one.
            code = getattr(exc, "code", None)
            if isinstance(code, str) and code not in EXECUTION_TRANSPORT_FAILURE_CODES:
                refusal(code, str(exc) or "execution owner rejected the operation",
                        getattr(exc, "details", None), None)
            raise
        if isinstance(response, dict) and response.get("ok") is False:
            error = _refusal(response)
            if error.code in EXECUTION_TRANSPORT_FAILURE_CODES:
                raise error
            refusal(error.code, error.message, error.details, response.get("active_tcp"))
        return self._successful(response, label="submission")

    def _planning_job_call(self, command: str, members: Mapping[str, Any]) -> dict[str, Any]:
        """One planning-job exchange; a refusal raises with its typed code."""

        response = self._send(command, members)
        if isinstance(response, Mapping) and response.get("ok") is False:
            error = response.get("error")
            if isinstance(error, Mapping) and isinstance(error.get("code"), str) and error["code"]:
                raise KernelPlanningJobRefusedError(
                    error["code"],
                    str(error.get("message") or error["code"]),
                    error.get("details") if isinstance(error.get("details"), Mapping) else None,
                )
        response = self._successful(response, label=command.replace("_", " "))
        if command != "release_planning_job" and not isinstance(response.get("planning_job"), Mapping):
            raise KernelClientResponseError(f"execution owner {command} carries no planning job")
        return response

    def plan_pick(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        """Accepted at once with the planning job (OD-213 (1))."""

        return self._planning_job_call("plan_pick", {"intent": _planning_intent_wire(intent)})

    def plan_place(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        """Accepted at once with the planning job (OD-213 (1))."""

        return self._planning_job_call("plan_place", {"intent": _planning_intent_wire(intent)})

    def read_planning_job(self, job_id: str) -> dict[str, Any]:
        return self._planning_job_call("read_planning_job", {"job_id": job_id})

    def cancel_planning_job(self, job_id: str) -> dict[str, Any]:
        """This job only; the host's control socket (never queued)."""

        return self._planning_job_call("cancel_planning_job", {"job_id": job_id})

    def release_planning_job(self, job_id: str) -> dict[str, Any]:
        return self._planning_job_call("release_planning_job", {"job_id": job_id})

    def planning_job_state(self, job_id: str) -> str | None:
        """The job's state from the pushed copy (memory only); None while the
        copy does not list it yet. Raises
        ``KernelOperationObservationUnavailableError`` while the copy is
        unavailable. A process without the event subscription reads it."""

        events = self._events
        if events is None:
            job = self.read_planning_job(job_id)["planning_job"]
            state = job.get("state")
            return state if state in _PLANNING_JOB_STATES else None
        return _pushed_read(lambda: events.planning_job_state(job_id))

    def operation(self, operation_id: int) -> dict[str, Any]:
        try:
            return self._owner("read", {"operation_id": operation_id})
        except Exception as exc:
            # Only owner availability on this exact read is mapped; every
            # other transport, protocol and owner error keeps its type.
            if getattr(exc, "code", None) == "execution_owner_unavailable":
                raise KernelOperationObservationUnavailableError(
                    "native operation observation is temporarily unavailable"
                ) from exc
            raise

    def observed_operation(
        self,
        operation_id: int,
        after_change: object,
        *,
        wait: bool = True,
        read: bool = True,
    ) -> dict[str, Any] | None:
        """A ``read``-shaped envelope of an operation this process started,
        from the pushed table (OD-213) instead of a per-tick ``read``.

        ``after_change`` is the ``operations_change`` of the acceptance
        reply. While the copy predates it, ``wait`` raises
        ``KernelOperationNotYetPushedError`` (the node stays running); a
        one-shot caller passes ``wait=False`` and reads instead. An operation
        the current copy cannot show (retired, beyond the list's bound, or
        listed without its state) is read once. Without a pushed table this
        is ``operation``. ``read`` False (a Program tick, OD-380 D2): memory
        only, None where a ``read`` would be made.
        """

        events = self._events
        if events is None:
            return self.operation(operation_id) if read else None
        change = _counter(after_change)
        if change is None:
            raise KernelClientResponseError(
                "operation acceptance carries no operations change counter"
            )
        try:
            envelope = _pushed_read(lambda: events.operation_envelope(operation_id, change))
        except KernelOperationNotYetPushedError:
            if wait:
                raise
            return self.operation(operation_id) if read else None
        if envelope is None:
            return self.operation(operation_id) if read else None
        return envelope

    def recovery_assessment(self, operation_id: int) -> dict[str, Any]:
        response = self._owner("get_operation_recovery", {"operation_id": operation_id})
        assessment = response.get("recovery_assessment")
        if not isinstance(assessment, dict):
            raise KernelClientResponseError(
                "headless recovery assessment has no owner projection"
            )
        revision = response.get("operation_revision")
        participants = response.get("participants")
        if (
            type(revision) is not int
            or revision < 1
            or not isinstance(participants, list)
            or not participants
        ):
            raise KernelClientResponseError(
                "headless recovery assessment has no exact operation scope"
            )
        return {
            **assessment,
            "operation_revision": revision,
            "participants": participants,
            "stop_event_id": response.get("stop_event_id"),
        }

    def recover(
        self,
        operation_id: int,
        expected_operation_revision: int,
        request: Mapping[str, Any],
    ) -> dict[str, Any]:
        response = self._owner(
            "apply_operation_recovery",
            {
                "expected_operation_revision": expected_operation_revision,
                "operation_id": operation_id,
                "request": {"principal_id": "", "reason": "", **request},
            },
        )
        result = response.get("result")
        if (
            not isinstance(result, dict)
            or type(result.get("accepted")) is not bool
            or not isinstance(result.get("resulting_assessment"), dict)
        ):
            raise KernelClientResponseError(
                "headless recovery action has no exact owner result"
            )
        return result

    def query_operation(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._owner("query_operation", {"query": dict(request)})

    def cell_stop(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """The Program node's cell Stop (host ``cell_stop``): every operation
        in the cell, no hold change."""

        request = dict(request)
        if "request_context" in request:
            request["request_context"] = _context_wire(request["request_context"])
        return self._owner("cell_stop", {"request": request})

    def reset(self, operation_id: int, request: Mapping[str, Any]) -> dict[str, Any]:
        """``operation_id`` is the request's own ``operation_id``."""

        del operation_id
        return self._owner("reset", {"request": _control_wire(request)})

    def acknowledge_completed(
        self, operation_id: int, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        del operation_id
        try:
            return self._owner("acknowledge_completed", {"request": _control_wire(request)})
        except ExecutionCommandError as error:
            if error.code == "operation_effect_in_flight" or (
                error.details.get("reason") == "operation_effect_in_flight"
            ):
                raise KernelOperationSettlementPendingError(
                    "operation completion is waiting for a subordinate effect"
                ) from error
            raise

    def stop(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._owner("stop", {"request": _sparse_stop_wire(request)})

    def stop_program_run(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._owner("stop_program_run", {"request": _sparse_stop_wire(request)})

    def read_program_execution_scope(self, program_run_id: int) -> dict[str, Any]:
        scope = self._owner(
            "read_program_execution_scope", {"program_run_id": program_run_id}
        ).get("scope")
        if not isinstance(scope, dict):
            raise KernelClientResponseError("execution scope has an invalid shape")
        return scope

    def read_manual_handover(self, request: Mapping[str, Any]) -> dict[str, Any] | None:
        response = self._owner("read_manual_handover", {"operation": _operation_wire(request)})
        handover = response.get("handover")
        if handover is not None and not isinstance(handover, dict):
            raise KernelClientResponseError("manual handover has an invalid shape")
        return handover

    def stop_program_selection(self, request: Mapping[str, Any]) -> Mapping[str, Any]:
        response = self._owner("stop_program_selection", {"request": dict(request)})
        accepted = response.get("accepted")
        if (
            response.get("command") != "stop_program_selection"
            or not isinstance(accepted, Mapping)
            or not isinstance(accepted.get("group_ids"), list)
            or not accepted["group_ids"]
            or not isinstance(accepted.get("stop"), Mapping)
            or not isinstance(accepted.get("state"), Mapping)
        ):
            raise KernelClientResponseError("Program selection Stop omitted its scope")
        return accepted

    def release_program_selection(
        self, request: Mapping[str, Any], expected_hold_revision: int
    ) -> Mapping[str, Any]:
        response = self._selection_owner(
            "release_program_selection",
            {"request": dict(request), "expected_hold_revision": expected_hold_revision},
        )
        scope = response.get("scope")
        if (
            response.get("command") != "release_program_selection"
            or not isinstance(scope, Mapping)
            or type(scope.get("hold_revision")) is not int
        ):
            raise KernelClientResponseError(
                "Program selection Resume omitted its scope"
            )
        return scope

    def start_program_selection(
        self, request: Mapping[str, Any], expected_hold_revision: int
    ) -> Mapping[str, Any]:
        response = self._selection_owner(
            "start_program_selection",
            {"request": dict(request), "expected_hold_revision": expected_hold_revision},
        )
        scope = response.get("scope")
        if (
            response.get("command") != "start_program_selection"
            or not isinstance(scope, Mapping)
            or type(scope.get("hold_revision")) is not int
        ):
            raise KernelClientResponseError(
                "Program selection initial Run omitted its scope"
            )
        return scope

    def check_program_node_motion_attention(
        self, request: Mapping[str, Any]
    ) -> None:
        response = self._selection_owner(
            "check_program_node_motion_attention", {"request": dict(request)}
        )
        if response.get("command") != "check_program_node_motion_attention":
            raise KernelClientResponseError(
                "Program node motion attention omitted its command"
            )

    def release_halt(
        self, program_run_id: int, expected_hold_revision: int
    ) -> Mapping[str, Any] | None:
        """Execute after Halt: lower the run's frozen level at exactly
        ``expected_hold_revision``. Returns the scope the reply carries (its
        ``hold_revision`` is the revision after the release); None when the
        run is not the Kernel's current run (no effect)."""

        response = self._selection_owner(
            "release_halt",
            {"expected_hold_revision": expected_hold_revision, "program_run_id": program_run_id},
        )
        scope = response.get("scope")
        # The reply always carries `scope`; null means the run is not the
        # Kernel's current run. A reply without the key is malformed.
        if response.get("command") == "release_halt" and "scope" in response and scope is None:
            return None
        if (
            response.get("command") != "release_halt"
            or not isinstance(scope, Mapping)
            or type(scope.get("hold_revision")) is not int
        ):
            raise KernelClientResponseError("Halt release omitted its scope")
        return scope

    def read_stop(self, event_id: int) -> dict[str, Any]:
        """The Stop event record (``manyforge.execution_stop_status.v2``) of
        exactly ``event_id``; an overwritten record is the host's
        ``stop_status_not_resident`` refusal."""

        reply = self._send("read_stop", {"event_id": event_id})
        if isinstance(reply, dict) and reply.get("ok") is False:
            error = reply.get("error")
            details = error.get("details") if isinstance(error, Mapping) else None
            if isinstance(error, Mapping) and (
                error.get("code") == "stop_status_not_resident"
                or isinstance(details, Mapping)
                and details.get("reason") == "stop_status_not_resident"
            ):
                raise KernelStopNotResidentError(event_id)
            raise _refusal(reply)
        status = self._successful(reply, label="Stop status").get("stop_status")
        if (
            not isinstance(status, dict)
            or status.get("schema_version") != "manyforge.execution_stop_status.v2"
            or status.get("event_id") != event_id
            or status.get("state") not in {"pending", "complete"}
        ):
            raise KernelClientResponseError(
                "execution Stop status changed its event identity"
            )
        return status

    def set_rule_active(self, rule_id: str, active: bool) -> dict[str, Any]:
        """Kernel `set_rule_active`; `rule_unknown` is refused."""

        if not isinstance(rule_id, str) or not rule_id or type(active) is not bool:
            raise ValueError("set_rule_active requires a rule id and a boolean")
        return self._owner("set_rule_active", {"rule_id": rule_id, "active": active})

    def reset_rule_activation(self) -> dict[str, Any]:
        """Every global rule returns to its `active_at_start` (run end, Reset)."""

        return self._owner("reset_rule_activation")

    def clear_outside_change(
        self, controllable_component_id: str, signal_name: str
    ) -> dict[str, Any]:
        """Kernel `clear_outside_change`; writes nothing."""

        return self._owner(
            "clear_outside_change",
            {"controllable_component_id": controllable_component_id, "signal_name": signal_name},
        )

    def set_io_output(
        self, component_id: str, signal_name: str, value: bool | float
    ) -> dict[str, Any]:
        """One output write by signal id through the one write path, sent
        once. A refusal (`cell_latched`, `rule_holds`) raises the host's
        typed error; a `failed` or `unknown` result raises
        `IoOutputWriteFailedError` with the host's code."""

        response = self._owner(
            "set_io_output",
            {"request": {"signal_id": f"{component_id}/{signal_name}", "value": value,
                         "origin": "program"}},
        )
        result = response["result"]
        if result["outcome"] == "succeeded":
            return result
        error = result.get("error")
        if not isinstance(error, Mapping) or not isinstance(error.get("code"), str):
            raise KernelClientResponseError(
                f"output write ended {result['outcome']} without a typed error"
            )
        raise IoOutputWriteFailedError(
            error["code"], str(error.get("message") or error["code"]), result["outcome"]
        )


class MockKernelClient:
    """Removed stateful semantic mock; tests fake the thin HTTP transport."""

    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise LegacyKernelMockUnavailableError(
            "stateful Python MockKernelClient was superseded by the C++ "
            "resource-addressed owner; inject a bounded transport fake"
        )


__all__ = [
    "ActionSpan",
    "EXECUTION_TRANSPORT_FAILURE_CODES",
    "ExecutionState",
    "ExecutionCommandError",
    "IoOutputWriteFailedError",
    "KernelClient",
    "KernelOperationAdmissionRejectedError",
    "KernelImmediateStepFailedError",
    "KernelOperationNotYetPushedError",
    "KernelOperationObservationUnavailableError",
    "KernelPlanningJobRefusedError",
    "PLANNING_JOB_FINAL_STATES",
    "KernelOperationSettlementPendingError",
    "KernelStopNotResidentError",
    "KernelClientResponseError",
    "LegacyKernelMockUnavailableError",
    "MockKernelClient",
]
