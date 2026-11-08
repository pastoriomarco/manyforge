"""Thin Behavior client for the resource-addressed native execution owner."""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, Mapping, Protocol


class ExecutionTransport(Protocol):
    """Bounded operation-addressed calls to the execution owner."""

    def identity(self) -> dict[str, Any]: ...

    def targets(self) -> dict[str, Any]: ...

    def control_state(self) -> dict[str, Any]: ...

    def observation(self) -> dict[str, Any]: ...

    def query_target(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def query_operation(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def register_io_watch(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def read_io_watch(self, registration: Mapping[str, Any]) -> dict[str, Any]: ...

    def release_io_watch(self, registration: Mapping[str, Any]) -> None: ...

    def release_io_watch_scope(self, scope_request: Mapping[str, Any]) -> None: ...

    def start_mock_io_node_simulation(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def end_mock_io_node_simulation(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def set_mock_io_run_paused(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def full_stop(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def set_operation_runtime_scale(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def submit(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def register_program_run_capture(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def read_program_execution_scope(self, program_run_id: str) -> dict[str, Any]: ...

    def read_manual_handover(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def stop_program_selection(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def release_program_selection(
        self, body: Mapping[str, Any], expected_selection_revision: int
    ) -> dict[str, Any]: ...

    def start_program_selection(
        self, body: Mapping[str, Any], expected_selection_revision: int
    ) -> dict[str, Any]: ...

    def check_program_node_motion_attention(
        self, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def read(self, operation_id: str) -> dict[str, Any]: ...

    def operation_recovery(self, operation_id: str) -> dict[str, Any]: ...

    def apply_operation_recovery(
        self,
        operation_id: str,
        expected_operation_revision: int,
        body: Mapping[str, Any],
    ) -> dict[str, Any]: ...

    def reset(self, operation_id: str, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def acknowledge_completed(
        self, operation_id: str, body: Mapping[str, Any]
    ) -> dict[str, Any]: ...

    def stop(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def stop_program_run(self, body: Mapping[str, Any]) -> dict[str, Any]: ...

    def release_program_run_stop_fence(
        self, program_run_id: str, stop_id: str, stop_generation: int
    ) -> dict[str, Any]: ...

    def stop_status(self, stop_id: str) -> dict[str, Any]: ...

    def set_rule_active(self, rule_id: str, active: bool) -> dict[str, Any]: ...

    def reset_rule_activation(self) -> dict[str, Any]: ...

    def clear_outside_change(
        self, controllable_component_id: str, signal_name: str
    ) -> dict[str, Any]: ...

    def set_io_output(
        self, signal_id: str, value: bool | float, *, origin: str
    ) -> dict[str, Any]: ...


# Transport loss and malformed replies: they cannot prove what
# the owner did, so they are never an owner refusal (or a teardown).
EXECUTION_TRANSPORT_FAILURE_CODES = frozenset(
    {
        "execution_owner_unavailable",
        "execution_command_transport_failure",
        "execution_command_response_invalid",
    }
)


class KernelClientResponseError(RuntimeError):
    """Raised when a transport returns a successful but malformed projection."""


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
    program_run_id: str | None = None
    node_instance_id: str | None = None
    node_attempt_id: str | None = None
    parent_node_attempt_id: str | None = None

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
            if self.action_span_id != self.node_attempt_id:
                raise ValueError(
                    "program_node action_span_id must equal node_attempt_id"
                )

    def as_request(self) -> dict[str, str | None]:
        return {
            "action_source": self.action_source,
            "action_span_id": self.action_span_id,
            "program_run_id": self.program_run_id,
            "node_instance_id": self.node_instance_id,
            "node_attempt_id": self.node_attempt_id,
            "parent_node_attempt_id": self.parent_node_attempt_id,
        }


class KernelClient:
    """Stateless adapter; the C++ host remains the only execution owner."""

    def __init__(self, transport: ExecutionTransport) -> None:
        for method in (
            "identity",
            "submit",
            "read",
            "reset",
            "acknowledge_completed",
            "stop",
        ):
            if not callable(getattr(transport, method, None)):
                raise TypeError(f"execution transport has no callable {method}")
        self._transport = transport

    @staticmethod
    def _successful(value: object, *, label: str) -> dict[str, Any]:
        if not isinstance(value, dict) or value.get("ok") is not True:
            raise KernelClientResponseError(
                f"execution owner {label} did not return a successful envelope"
            )
        return value

    def identity(self) -> dict[str, Any]:
        return self._successful(self._transport.identity(), label="identity")

    def register_program_run_capture(self, body: Mapping[str, Any]) -> dict[str, Any]:
        """Install a claim-bound capture through the private Program transport."""

        return self._successful(
            self._transport.register_program_run_capture(body),
            label="Program execution capture",
        )

    def targets(self) -> dict[str, Any]:
        return self._projection("targets")

    def read_active_tcp(self, *, ordered: bool = False) -> dict[str, Any]:
        if type(ordered) is not bool:
            raise TypeError("ordered must be boolean")
        method = getattr(self._transport, "read_active_tcp", None)
        if not callable(method):
            raise TypeError("execution transport has no callable read_active_tcp")
        return self._successful(
            method(ordered=True) if ordered else method(), label="read active tcp"
        )

    def reset_active_tcp(self) -> dict[str, Any]:
        return self._projection("reset_active_tcp")

    def control_state(self) -> dict[str, Any]:
        return self._projection("control_state")

    def observation(self) -> dict[str, Any]:
        return self._projection("observation")

    def query_target(self, body: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.query_target(body), label="target observation"
        )

    def _io_watch_method(self, name: str):
        method = getattr(self._transport, name, None)
        if not callable(method):
            raise TypeError(f"execution transport has no callable {name}")
        return method

    def register_io_watch(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """Register one zero-claim level watch: ``{registration, result}``."""

        response = self._io_watch_method("register_io_watch")(request)
        if (
            not isinstance(response, dict)
            or not isinstance(response.get("registration"), dict)
            or not isinstance(response.get("result"), dict)
        ):
            raise KernelClientResponseError("I/O watch registration is malformed")
        return response

    def read_io_watch(self, registration: Mapping[str, Any]) -> dict[str, Any]:
        """The watch's current ``{state, observation, matched_sample}``."""

        result = self._io_watch_method("read_io_watch")(registration)
        if not isinstance(result, dict) or result.get("state") not in {
            "pending", "matched", "released"
        }:
            raise KernelClientResponseError("I/O watch read is malformed")
        return result

    def release_io_watch(self, registration: Mapping[str, Any]) -> None:
        self._io_watch_method("release_io_watch")(registration)

    def release_io_watch_scope(self, scope_request: Mapping[str, Any]) -> None:
        """Release one exact node attempt's watch without its token."""

        self._io_watch_method("release_io_watch_scope")(scope_request)

    def start_mock_io_node_simulation(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """The host's exact accepted acknowledgement."""

        ack = self._io_watch_method("start_mock_io_node_simulation")(request)
        if (
            not isinstance(ack, dict)
            or ack.get("status") != "accepted"
            or not isinstance(ack.get("signals"), list)
        ):
            raise KernelClientResponseError("Mock simulation acknowledgement is malformed")
        return ack

    def end_mock_io_node_simulation(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """The host's reply to one node attempt's end."""

        reply = self._io_watch_method("end_mock_io_node_simulation")(request)
        if not isinstance(reply, dict):
            raise KernelClientResponseError("Mock simulation end reply is malformed")
        return reply

    def set_mock_io_run_paused(self, request: Mapping[str, Any]) -> dict[str, Any]:
        """The host's reply to the Run's Halt state."""

        reply = self._io_watch_method("set_mock_io_run_paused")(request)
        if not isinstance(reply, dict):
            raise KernelClientResponseError("Mock Run pause reply is malformed")
        return reply

    def _projection(self, method_name: str) -> dict[str, Any]:
        method = getattr(self._transport, method_name, None)
        if not callable(method):
            raise TypeError(f"execution transport has no callable {method_name}")
        return self._successful(method(), label=method_name.replace("_", " "))

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
                expected = context.get("request_id") if isinstance(context, Mapping) else None
                if (not isinstance(expected, str) or not expected
                    or detail.get("request_id") != expected
                    or not isinstance(detail.get("operation_id"), str)
                    or not detail["operation_id"]):
                    raise KernelClientResponseError("immediate-step failure receipt is invalid")
                raise KernelImmediateStepFailedError(code, message, detail, active_tcp)
            raise KernelOperationAdmissionRejectedError(code, message, detail)

        try:
            response = self._transport.submit(intent)
        except Exception as exc:
            code = getattr(exc, "code", None)
            if isinstance(code, str) and code not in EXECUTION_TRANSPORT_FAILURE_CODES:
                refusal(code, str(exc) or "execution owner rejected the operation",
                        getattr(exc, "details", None), getattr(exc, "active_tcp", None))
            raise
        if isinstance(response, dict) and response.get("ok") is False:
            error = response.get("error")
            if (
                isinstance(error, Mapping)
                and isinstance(error.get("code"), str)
                and error["code"]
                and isinstance(error.get("message"), str)
                and isinstance(error.get("details", {}), Mapping)
            ):
                refusal(error["code"], error["message"], error.get("details"),
                        response.get("active_tcp"))
        return self._successful(response, label="submission")

    def plan_pick(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "plan_pick", None)
        if not callable(method):
            raise TypeError("execution transport has no callable plan_pick")
        return self._successful(method(intent), label="pick planning")

    def plan_place(self, intent: Mapping[str, Any]) -> dict[str, Any]:
        method = getattr(self._transport, "plan_place", None)
        if not callable(method):
            raise TypeError("execution transport has no callable plan_place")
        return self._successful(method(intent), label="place planning")

    def operation(self, operation_id: str) -> dict[str, Any]:
        try:
            response = self._transport.read(operation_id)
        except Exception as exc:
            # Headless transports expose stable machine-readable error codes.
            # Map only owner availability on this exact read; every other
            # transport, protocol, and owner error retains its original type.
            if getattr(exc, "code", None) == "execution_owner_unavailable":
                raise KernelOperationObservationUnavailableError(
                    "native operation observation is temporarily unavailable"
                ) from exc
            raise
        return self._successful(response, label="observation")

    def recovery_assessment(self, operation_id: str) -> dict[str, Any]:
        method = getattr(self._transport, "operation_recovery", None)
        if not callable(method):
            raise TypeError("execution transport has no callable operation_recovery")
        response = self._successful(method(operation_id), label="recovery assessment")
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
            "stop_id": response.get("stop_id"),
            "stop_generation": response.get("stop_generation"),
        }

    def recover(
        self,
        operation_id: str,
        expected_operation_revision: int,
        request: Mapping[str, Any],
    ) -> dict[str, Any]:
        method = getattr(self._transport, "apply_operation_recovery", None)
        if not callable(method):
            raise TypeError(
                "execution transport has no callable apply_operation_recovery"
            )
        response = self._successful(
            method(operation_id, expected_operation_revision, request),
            label="recovery action",
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
        return self._successful(
            self._transport.query_operation(request), label="Program operation context"
        )

    def full_stop(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(self._transport.full_stop(request), label="Full Stop")

    def set_operation_runtime_scale(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.set_operation_runtime_scale(request),
            label="operation runtime scale",
        )

    def reset(self, operation_id: str, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.reset(operation_id, request),
            label="Reset",
        )

    def acknowledge_completed(
        self, operation_id: str, request: Mapping[str, Any]
    ) -> dict[str, Any]:
        try:
            response = self._transport.acknowledge_completed(operation_id, request)
        except Exception as error:
            details = getattr(error, "details", None)
            if getattr(error, "code", None) == "operation_effect_in_flight" or (
                isinstance(details, Mapping)
                and details.get("reason") == "operation_effect_in_flight"
            ):
                raise KernelOperationSettlementPendingError(
                    "operation completion is waiting for a subordinate effect"
                ) from error
            raise
        return self._successful(response, label="completion acknowledgement")

    def stop(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.stop(request),
            label="Stop",
        )

    def stop_program_run(self, request: Mapping[str, Any]) -> dict[str, Any]:
        return self._successful(
            self._transport.stop_program_run(request),
            label="Program-run Stop",
        )

    def read_program_execution_scope(self, program_run_id: str) -> dict[str, Any]:
        response = self._successful(
            self._transport.read_program_execution_scope(program_run_id),
            label="Program execution scope",
        )
        scope = response.get("scope")
        if not isinstance(scope, dict):
            raise KernelClientResponseError("execution scope has an invalid shape")
        return scope

    def read_manual_handover(self, request: Mapping[str, Any]) -> dict[str, Any] | None:
        response = self._successful(
            self._transport.read_manual_handover(request), label="manual handover"
        )
        handover = response.get("handover")
        if handover is not None and not isinstance(handover, dict):
            raise KernelClientResponseError("manual handover has an invalid shape")
        return handover

    def stop_program_selection(self, request: Mapping[str, Any]) -> Mapping[str, Any]:
        response = self._successful(
            self._transport.stop_program_selection(request),
            label="Program selection Stop",
        )
        accepted = response.get("accepted")
        if (
            response.get("command") != "stop_program_selection"
            or not isinstance(accepted, Mapping)
            or not isinstance(accepted.get("group_ids"), list)
            or not accepted["group_ids"]
            or accepted.get("replay_status") not in {"accepted_new", "replayed_exact"}
            or not isinstance(accepted.get("state"), Mapping)
        ):
            raise KernelClientResponseError("Program selection Stop omitted its scope")
        return accepted

    def release_program_selection(
        self, request: Mapping[str, Any], expected_selection_revision: int
    ) -> Mapping[str, Any]:
        response = self._successful(
            self._transport.release_program_selection(
                request, expected_selection_revision
            ),
            label="Program selection Resume",
        )
        scope = response.get("scope")
        if (
            response.get("command") != "release_program_selection"
            or not isinstance(scope, Mapping)
            or type(scope.get("selection_revision")) is not int
        ):
            raise KernelClientResponseError(
                "Program selection Resume omitted its scope"
            )
        return scope

    def start_program_selection(
        self, request: Mapping[str, Any], expected_selection_revision: int
    ) -> Mapping[str, Any]:
        response = self._successful(
            self._transport.start_program_selection(
                request, expected_selection_revision
            ),
            label="Program selection initial Run",
        )
        scope = response.get("scope")
        if (
            response.get("command") != "start_program_selection"
            or not isinstance(scope, Mapping)
            or type(scope.get("selection_revision")) is not int
        ):
            raise KernelClientResponseError(
                "Program selection initial Run omitted its scope"
            )
        return scope

    def check_program_node_motion_attention(
        self, request: Mapping[str, Any]
    ) -> None:
        response = self._successful(
            self._transport.check_program_node_motion_attention(request),
            label="Program node motion attention",
        )
        if response.get("command") != "check_program_node_motion_attention":
            raise KernelClientResponseError(
                "Program node motion attention omitted its command"
            )

    def release_program_run_stop_fence(
        self, program_run_id: str, stop_id: str, stop_generation: int
    ) -> dict[str, Any]:
        return self._successful(
            self._transport.release_program_run_stop_fence(
                program_run_id, stop_id, stop_generation
            ),
            label="Program-run Stop release",
        )

    def stop_status(self, stop_id: str) -> dict[str, Any]:
        method = getattr(self._transport, "stop_status", None)
        if not callable(method):
            raise TypeError("execution transport has no callable stop_status")
        response = self._successful(method(stop_id), label="Stop status")
        status = response.get("stop_status")
        if (
            not isinstance(status, dict)
            or status.get("stop_id") != stop_id
            or status.get("state")
            not in {
                "accepted",
                "stopping",
                "certifying",
                "succeeded",
                "partial",
                "failed",
            }
        ):
            raise KernelClientResponseError(
                "execution Stop status changed its aggregate identity"
            )
        return response

    def _required(self, name: str) -> Callable[..., Any]:
        method = getattr(self._transport, name, None)
        if not callable(method):
            raise TypeError(f"execution transport has no callable {name}")
        return method

    def set_rule_active(self, rule_id: str, active: bool) -> dict[str, Any]:
        """Kernel `set_rule_active`; `rule_unknown` is refused."""

        if not isinstance(rule_id, str) or not rule_id or type(active) is not bool:
            raise ValueError("set_rule_active requires a rule id and a boolean")
        return self._successful(
            self._required("set_rule_active")(rule_id, active), label="rule activation"
        )

    def reset_rule_activation(self) -> dict[str, Any]:
        """Every global rule returns to its `active_at_start` (run end, Reset)."""

        return self._successful(
            self._required("reset_rule_activation")(), label="rule activation reset"
        )

    def clear_outside_change(
        self, controllable_component_id: str, signal_name: str
    ) -> dict[str, Any]:
        """Kernel `clear_outside_change`; writes nothing."""

        return self._successful(
            self._required("clear_outside_change")(controllable_component_id, signal_name),
            label="outside change clear",
        )

    def set_io_output(
        self, component_id: str, signal_name: str, value: bool | float
    ) -> dict[str, Any]:
        """One output write by signal id through the one write path, sent
        once. A refusal (`cell_latched`, `rule_holds`) raises the
        transport's typed error; a `failed` or `unknown` result raises
        `IoOutputWriteFailedError` with the host's code."""

        response = self._successful(
            self._required("set_io_output")(
                f"{component_id}/{signal_name}", value, origin="program"
            ),
            label="output write",
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
    "ExecutionTransport",
    "IoOutputWriteFailedError",
    "KernelClient",
    "KernelOperationAdmissionRejectedError",
    "KernelImmediateStepFailedError",
    "KernelOperationObservationUnavailableError",
    "KernelOperationSettlementPendingError",
    "KernelClientResponseError",
    "LegacyKernelMockUnavailableError",
    "MockKernelClient",
]
