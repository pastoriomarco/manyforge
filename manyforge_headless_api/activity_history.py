"""History records of component actions, Apply and Program Reset terminals,
Program node attempts, scene changes, operator commands and manual output
writes.

One event per finished component action (whoever asked for it), one per
Scenario Load / New, Deployment Apply / Revert and Program Apply / Revert
terminal, one per Program Reset terminal (OD-305), one per finished Program
node attempt, one per scene change made outside a Program node
(OD-375, OD-378), one per operator command at its answer (identical
refusals counted into one row) and one per manual output write. Each record
carries the typed outcome and error code; ``message`` is the History list's
display line, built here from those typed fields. A record built while a
request is served may name its caller (request id, transport, request key,
source label, principal). The native historian validates the record and
stores it in the existing bounded History (no store of its own).
"""

from __future__ import annotations

from collections.abc import Mapping
from contextvars import ContextVar, Token
import json
import logging
import math
from threading import Lock
import time
from typing import Any, Callable, Final

import rfc8785

from manyforge_behavior.failure_details import MAX_FAILURE_DETAILS_BYTES

from .control_protocol import is_stop_command
from .execution_control import ExecutionControlBusyError
from .identity import format_request_key, http_request_key, websocket_request_key

SCHEMA_VERSION: Final = "manyforge.activity_event.v1"
APPLY_KINDS: Final = frozenset({
    "scenario_load", "scenario_new", "deployment_apply", "deployment_revert",
    "program_apply", "program_revert",
})
PROGRAM_RESET_OUTCOMES: Final = frozenset({"succeeded", "refused", "failed", "unknown"})
PROGRAM_RESET_FIELDS: Final = (
    "reload_initial_scene", "reset_robot_position", "reset_stored_values",
    "program_run_id", "program_revision", "applied_record_digest",
    "scene_revision_id", "scene_content_hash", "cause_code",
)
PROGRAM_NODE_ATTEMPT_OUTCOMES: Final = frozenset({
    "succeeded", "failed", "interrupted", "stopped", "preempted",
})
# A node attempt carries its typed error only when it failed or was interrupted.
PROGRAM_NODE_ATTEMPT_ERROR_OUTCOMES: Final = frozenset({"failed", "interrupted"})
PROGRAM_NODE_ATTEMPT_FIELDS: Final = (
    "program_run_id", "node_instance_id", "node_attempt_id", "started_at_wall",
    "finished_at_ns", "finished_at_wall", "duration_ms", "scene_operation",
    "scene_resource_id",
)
SCENE_CHANGE_OUTCOMES: Final = frozenset({"succeeded", "refused", "failed", "unknown"})
SCENE_CHANGE_FIELDS: Final = (
    "scene_resource_id", "source_label", "principal_id", "duration_ms",
    "scene_revision_id",
)
# The host's `component_action_interrupted_by_stop` failure reads as interrupted.
INTERRUPTED_CODE: Final = "component_action_interrupted_by_stop"
CALLER_FIELDS: Final = (
    "request_id", "request_key", "transport", "source_label", "principal_id",
)
COMMAND_OUTCOMES: Final = frozenset({"succeeded", "accepted", "refused", "failed", "unknown"})
IO_OUTPUT_OUTCOMES: Final = frozenset({"succeeded", "refused", "failed", "unknown"})
# An identical refusal (same command, code, transport and source label), or
# an accepted runtime scale change from the same source label and transport,
# within this time of the last row written for it is counted, not written;
# the next row written for it carries the count.
COMMAND_REPEAT_WINDOW_NS: Final = 10_000_000_000
# At most this many counted runs (refusals and accepted runtime scale
# changes) are kept at once (memory only); the oldest is forgotten first.
MAXIMUM_COUNTED_REFUSALS: Final = 128
# The History name of each control command kind.
CONTROL_COMMAND_NAMES: Final = {
    "program_run": "execute",
    "program_motion_start": "motion_start",
    "program_motion_stop": "motion_stop",
    "program_motion_resume": "resume",
    "program_reset": "program_reset",
    "program_resume_halt": "resume_halt",
    "program_discard_halt": "discard_halt",
    "program_runtime_blackboard_write": "blackboard_write",
    "program_manual_result_stage": "manual_result_stage",
    "program_manual_handover": "manual_handover",
    "operation_start": "operation_start",
    "operation_stop": "stop",
    "operation_reset": "operation_reset",
    "operation_acknowledge_completed": "operation_acknowledge",
    "stop_all": "stop_all",
    "halt": "halt",
    "execution_recovery": "recovery_action",
    "runtime_scale": "runtime_scale",
}
# The command name of each recognized Stop route.
STOP_ROUTE_COMMANDS: Final = {
    "/api/execution/stops": "stop",
    "/api/execution/stop-all": "stop_all",
    "/api/execution/halt": "halt",
}
_ERROR_FIELDS: Final = ("code", "message", "vendor_code", "vendor_text")
_SAFE_INTEGER: Final = 9_007_199_254_740_991
_LOGGER = logging.getLogger(__name__)
# The request being served (Headless's HTTP middleware): its id and transport.
# Worker threads, background threads and the execution-control lanes run
# without it, so what they record names no request.
_REQUEST_CALLER: ContextVar[dict[str, str] | None] = ContextVar(
    "manyforge_activity_request_caller", default=None
)


_TEXT_BYTES: Final = 2048


def _bounded(value: Any, limit: int = _TEXT_BYTES) -> str:
    return str(value).encode("utf-8")[:limit].decode("utf-8", errors="ignore")


def _error(error: Any) -> dict[str, str] | None:
    if not isinstance(error, Mapping) or not error.get("code"):
        return None
    return {
        name: _bounded(error[name]) for name in _ERROR_FIELDS if error.get(name)
    }


def _typed_error(error: Any) -> dict[str, Any] | None:
    """A typed error with its structured details, kept when they fit the run
    records' failure-details bound."""

    typed = _error(error)
    details = error.get("details") if typed is not None else None
    if isinstance(details, Mapping) and details:
        try:
            fits = len(rfc8785.dumps(dict(details))) <= MAX_FAILURE_DETAILS_BYTES
        except (rfc8785.CanonicalizationError, TypeError, ValueError):
            fits = False
        if fits:
            typed["details"] = dict(details)
    return typed


def _words(identifier: str) -> str:
    return identifier.replace("_", " ")


def _line(subject: str, outcome: str, error: dict[str, str] | None) -> str:
    line = f"{subject} {outcome}"
    if error is not None:
        line += f": {error['code']}"
        if error.get("message"):
            line += f": {error['message']}"
        if error.get("vendor_code"):
            line += f" (vendor {error['vendor_code']})"
    return line


def component_action_record(
    component_id: str, action: str, outcome: str, error: Any = None
) -> dict[str, Any]:
    """One finished (or refused) component action."""

    typed = _error(error)
    if outcome == "failed" and typed is not None and typed["code"] == INTERRUPTED_CODE:
        outcome = "interrupted"
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "component_action",
        "subject": component_id,
        "action": action,
        "outcome": outcome,
        "error": typed,
        "components": [],
        "message": _bounded(
            _line(f"{_words(action).capitalize()} {component_id}", outcome, typed), 4096
        ),
    }


def apply_record(
    apply_kind: str,
    outcome: str,
    error: Any = None,
    components: Any = (),
) -> dict[str, Any]:
    """One Apply terminal with its component rows (ApplyResult)."""

    typed = _error(error)
    rows: list[dict[str, Any]] = []
    for row in components or ():
        if not isinstance(row, Mapping) or not row.get("component_id"):
            continue
        kept: dict[str, Any] = {"component_id": str(row["component_id"])}
        for name in ("disposition", "lifecycle_after"):
            if row.get(name):
                kept[name] = str(row[name])
        if (row_error := _error(row.get("error"))) is not None:
            kept["error"] = row_error
        rows.append(kept)
    failed_rows = [row for row in rows if "error" in row]
    message = _line(_words(apply_kind).capitalize(), outcome, typed)
    if failed_rows:
        message += "; " + "; ".join(
            f"{row['component_id']}: {row['error']['code']}" for row in failed_rows
        )
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "apply",
        "subject": apply_kind,
        "action": None,
        "outcome": outcome,
        "error": typed,
        "components": rows,
        "message": _bounded(message, 4096),
    }


def program_reset_record(
    outcome: str, error: Any, reset: Mapping[str, Any]
) -> dict[str, Any]:
    """One Program Reset terminal: its options, the run it reset, the Applied
    Program it used and the scene revision its verified readback left."""

    typed = _error(error)
    facts = {name: reset.get(name) for name in PROGRAM_RESET_FIELDS}
    parts = [_line("Program reset", outcome, typed)]
    if facts["program_run_id"] is not None:
        parts.append(f"run {facts['program_run_id']}")
    if facts["scene_revision_id"] is not None:
        parts.append(f"reloaded initial scene (scene revision {facts['scene_revision_id']})")
    elif facts["reload_initial_scene"]:
        parts.append("reload initial scene")
    if facts["reset_robot_position"]:
        parts.append("reset robot position")
    if facts["reset_stored_values"]:
        parts.append("reset stored values")
    if facts["cause_code"] is not None:
        parts.append(f"cause {facts['cause_code']}")
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "program_reset",
        "subject": "program_reset",
        "action": None,
        "outcome": outcome,
        "error": typed,
        "components": [],
        "message": _bounded(" · ".join(parts), 4096),
        "reset": facts,
    }


def _duration(duration_ms: float) -> str:
    return f"{duration_ms:.0f} ms" if duration_ms < 1000 else f"{duration_ms / 1000:.2f} s"


def _scene_target(operation: str | None, resource_id: str | None) -> str:
    return " ".join(part for part in (
        _words(operation) if operation else None, resource_id) if part)


def program_node_attempt_record(facts: Mapping[str, Any]) -> dict[str, Any]:
    """One finished Program node attempt: its run, instance, attempt, start,
    duration and typed result; for a scene node also the scene operation
    and object it changed."""

    outcome = facts["outcome"]
    typed = (
        _typed_error(facts.get("error"))
        if outcome in PROGRAM_NODE_ATTEMPT_ERROR_OUTCOMES
        else None
    )
    attempt = {name: facts.get(name) for name in PROGRAM_NODE_ATTEMPT_FIELDS}
    parts = [
        _line(f"Node {attempt['node_instance_id']} ({facts['node_id']})", outcome, typed),
        f"run {attempt['program_run_id']} attempt {attempt['node_attempt_id']}",
        _duration(attempt["duration_ms"]),
    ]
    if target := _scene_target(attempt["scene_operation"], attempt["scene_resource_id"]):
        parts.append(target)
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "program_node_attempt",
        "subject": facts["node_id"],
        "action": None,
        "outcome": outcome,
        "error": typed,
        "components": [],
        "message": _bounded(" · ".join(parts), 4096),
        "attempt": attempt,
    }


def scene_change_record(
    operation_kind: str, outcome: str, error: Any, scene_change: Mapping[str, Any]
) -> dict[str, Any]:
    """One scene change outside a Program node (GUI, assistant, MCP, any HTTP
    caller, the Program runner's own preparation and clears): the object, the
    caller, the time it took and the scene revision it committed."""

    typed = _typed_error(error) if outcome != "succeeded" else None
    facts = {name: scene_change.get(name) for name in SCENE_CHANGE_FIELDS}
    parts = [_line(
        f"Scene {_scene_target(operation_kind, facts['scene_resource_id'])}", outcome, typed
    )]
    if facts["source_label"]:
        parts.append(facts["source_label"])
    parts.append(_duration(facts["duration_ms"]))
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "scene_change",
        "subject": operation_kind,
        "action": None,
        "outcome": outcome,
        "error": typed,
        "components": [],
        "message": _bounded(" · ".join(parts), 4096),
        "scene_change": facts,
    }


def bind_request_caller(request_id: str, transport: str) -> Token:
    """Name the request this context serves (``transport``: http or mcp)."""

    return _REQUEST_CALLER.set({"request_id": request_id, "transport": transport})


def unbind_request_caller(token: Token) -> None:
    _REQUEST_CALLER.reset(token)


def caller(**known: Any) -> dict[str, str] | None:
    """The ``caller`` member: the request being served, with what the body
    or the session names (request key, source label, principal, transport).
    None when nothing is known."""

    named = {**(_REQUEST_CALLER.get() or {}), **known}
    kept = {
        name: _bounded(named[name], 256)
        for name in CALLER_FIELDS
        if isinstance(named.get(name), str) and named[name]
    }
    return kept or None


def with_caller(record: dict[str, Any], **known: Any) -> dict[str, Any]:
    """``record`` with its caller, when one is known (any kind)."""

    if (named := caller(**known)) is not None:
        record["caller"] = named
    return record


def _scalar_facts(facts: Mapping[str, Any]) -> dict[str, Any]:
    """The set facts of a command or output write: bounded text, numbers,
    booleans."""

    return {
        name: _bounded(value) if isinstance(value, str) else value
        for name, value in facts.items()
        if value is not None
    }


def command_finished_record(
    command: str, outcome: str, error: Any, facts: Mapping[str, Any]
) -> dict[str, Any]:
    """One operator command at its answer: accepted, done at once, or
    refused; ``facts`` carry ``repeat_count`` (identical refusals counted
    into this row)."""

    typed = _typed_error(error) if outcome not in {"succeeded", "accepted"} else None
    kept = _scalar_facts(facts)
    parts = [_line(_words(command).capitalize(), outcome, typed)]
    if kept.get("program_run_id") is not None:
        parts.append(f"run {kept['program_run_id']}")
    if kept.get("scale") is not None:
        parts.append(f"scale {kept['scale']:g}")
    if kept["repeat_count"] > 1:
        parts.append(f"{kept['repeat_count']} times")
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "command_finished",
        "subject": command,
        "action": None,
        "outcome": outcome,
        "error": typed,
        "components": [],
        "message": _bounded(" · ".join(parts), 4096),
        "command": kept,
    }


def io_output_record(
    signal_id: str, outcome: str, error: Any, io_output: Mapping[str, Any]
) -> dict[str, Any]:
    """One manual output write: the output, the value asked for, where it
    came from and the time it took."""

    typed = _typed_error(error) if outcome != "succeeded" else None
    facts = _scalar_facts(io_output)
    parts = [_line(
        f"Output {signal_id} set to {json.dumps(facts.get('value'))}", outcome, typed
    )]
    if "duration_ms" in facts:
        parts.append(_duration(facts["duration_ms"]))
    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "io_output_written",
        "subject": _bounded(signal_id, 256),
        "action": None,
        "outcome": outcome,
        "error": typed,
        "components": [],
        "message": _bounded(" · ".join(parts), 4096),
        "io_output": facts,
    }


def _exception_error(error: BaseException) -> tuple[str, dict[str, Any]]:
    """A command's raised refusal as an outcome and a typed error."""

    code = (
        "execution_control_ingress_busy"
        if isinstance(error, ExecutionControlBusyError)
        else getattr(error, "code", None)
    )
    message = str(error) or type(error).__name__
    if not isinstance(code, str) or not code:
        # As the control session answers it: the effect is not known.
        return "unknown", {"code": "execution_control_delivery_uncertain", "message": message}
    return "refused", {
        "code": code, "message": message, "details": getattr(error, "details", None),
    }


def _end_outcome(end: Mapping[str, Any]) -> tuple[str, dict[str, Any] | None]:
    """A runner command row's end, as a command's outcome."""

    if end["outcome"] == "succeeded":
        return "succeeded", None
    outcome = "failed" if end["outcome"] == "interrupted" else end["outcome"]
    return outcome, {"code": end["code"], "details": end.get("details")}


def runner_reply_outcome(reply: Mapping[str, Any]) -> tuple[str, dict[str, Any] | None]:
    """A runner command's answer (``program_command_reply.v1``): accepted,
    or the end its row already has."""

    end = reply["command"]["end"]
    return ("accepted", None) if end is None else _end_outcome(end)


def _safe_integer(value: Any) -> int | None:
    return value if type(value) is int and 0 <= value <= _SAFE_INTEGER else None


def _finite_number(value: Any) -> float | int | None:
    return value if type(value) in {int, float} and math.isfinite(value) else None


def _command_request_key(request: Any, receipt: Any) -> str | None:
    """The key the command was (or would have been) forwarded with."""

    if receipt is not None and receipt.request_key is not None:
        return receipt.request_key
    envelope = request.command_envelope
    try:
        if envelope.control_session_number is not None:
            return format_request_key(websocket_request_key(
                envelope.control_session_number, int(envelope.client_sequence)
            ))
        raw = request.payload.get("request_key")
        nested = request.payload.get("request_context")
        if raw is None and isinstance(nested, Mapping):
            raw = nested.get("request_key")
        return format_request_key(http_request_key(raw)) if raw is not None else None
    except (TypeError, ValueError):
        return None


class CommandHistory:
    """The ``command_finished`` records of one Headless.

    An identical refusal within ``COMMAND_REPEAT_WINDOW_NS`` of the last row
    written for it is counted, not written; the next row written for it
    carries ``repeat_count`` (the refusals since that row). Any other answer
    to the command, except an accepted runtime scale change, ends the run of
    identical refusals. Accepted runtime scale changes are counted the same
    way per source label and transport (a slider sends one per step); each
    written row carries its own, so the latest, ``scale``. Memory only, at
    most ``MAXIMUM_COUNTED_REFUSALS`` runs; ``record_activity`` never waits.
    """

    def __init__(self, core: Any, *, clock_ns: Callable[[], int] = time.monotonic_ns) -> None:
        self._core = core
        self._clock_ns = clock_ns
        # Leaf lock over the counted runs:
        # (command, code, transport, source label) -> [written at ns, counted];
        # code None for accepted runtime scale changes.
        self._lock = Lock()
        self._refusals: dict[tuple[str, str | None, str | None, str | None], list[int]] = {}

    def finished(
        self,
        command: str,
        outcome: str,
        error: Any = None,
        facts: Mapping[str, Any] | None = None,
        named_caller: Mapping[str, str] | None = None,
    ) -> bool:
        """Record one command's answer; True when a row was handed to History."""

        named_caller = dict(named_caller or {})
        with self._lock:
            now = self._clock_ns()
            counted = outcome == "refused" or (
                outcome == "accepted" and command == "runtime_scale"
            )
            if counted:
                key = (
                    command,
                    None if outcome == "accepted" else str(
                        error.get("code") if isinstance(error, Mapping) else None
                    ),
                    named_caller.get("transport"), named_caller.get("source_label"),
                )
                run = self._refusals.pop(key, None)
                if run is not None and now - run[0] < COMMAND_REPEAT_WINDOW_NS:
                    run[1] += 1
                    self._refusals[key] = run
                    return False
                repeat_count = 1 + (run[1] if run is not None else 0)
                if len(self._refusals) >= MAXIMUM_COUNTED_REFUSALS:
                    del self._refusals[next(iter(self._refusals))]
                self._refusals[key] = [now, 0]
            else:
                repeat_count = 1
                for key, run in self._refusals.items():
                    if key[0] == command:
                        # The next identical refusal is written at once.
                        run[0] = now - COMMAND_REPEAT_WINDOW_NS
        record = command_finished_record(
            command, outcome, error, {**(facts or {}), "repeat_count": repeat_count}
        )
        if named_caller:
            record["caller"] = named_caller
        return record_activity(self._core, record)

    def control_command(
        self,
        request: Any,
        *,
        receipt: Any = None,
        result: Any = None,
        error: BaseException | None = None,
        **known: Any,
    ) -> bool:
        """Record one control command (``ExecutionCommandRequestDTO``) at its
        receipt, or at the refusal it raised. An accepted Stop or Halt has its
        own stop event: for those only a refusal is recorded. A Program Reset
        the runner answered is recorded by the runner."""

        envelope = request.command_envelope
        kind = envelope.command_kind
        if error is not None:
            outcome, typed = _exception_error(error)
        elif receipt.disposition == "rejected":
            owner = result.get("error") if isinstance(result, Mapping) else None
            outcome, typed = "refused", (
                owner if isinstance(owner, Mapping) and owner.get("code") == receipt.code
                else {"code": receipt.code}
            )
        elif isinstance(receipt.command, Mapping) and receipt.command.get("end") is not None:
            outcome, typed = _end_outcome(receipt.command["end"])
        else:
            outcome, typed = "accepted", None
        if (outcome in {"accepted", "succeeded"} and is_stop_command(kind)) or (
            outcome == "succeeded" and kind == "runtime_scale"
        ):
            return False
        if kind == "program_reset" and (
            (receipt is not None and receipt.command is not None)
            or getattr(error, "producer", None) == "program_runner"
        ):
            return False
        payload = request.payload
        facts = {
            "command_kind": kind,
            "disposition": "rejected" if receipt is None else receipt.disposition,
            "receipt_code": (
                receipt.code
                if receipt is not None and outcome in {"accepted", "succeeded"}
                else None
            ),
            "control_session_number": envelope.control_session_number,
            "client_sequence": envelope.client_sequence,
            "program_run_id": _safe_integer(payload.get("program_run_id")),
            "operation_id": _safe_integer(payload.get("operation_id")),
            "scale": (
                _finite_number(payload.get("scale")) if kind == "runtime_scale" else None
            ),
        }
        return self.finished(
            CONTROL_COMMAND_NAMES[kind], outcome, typed, facts,
            caller(
                request_key=_command_request_key(request, receipt),
                source_label=envelope.source_label,
                **known,
            ),
        )


def record_activity(core: Any, record: dict[str, Any]) -> bool:
    """Hand one record to the historian; History is never the action's result.
    True when the historian took it."""

    try:
        return core.record_activity(record).get("recorded") is True
    except Exception:  # noqa: BLE001 - a lost History line never fails the action
        _LOGGER.warning(
            "activity_history_record_failed",
            extra={"kind": record.get("kind"), "subject": record.get("subject")},
            exc_info=True,
        )
        return False


__all__ = [
    "APPLY_KINDS",
    "CALLER_FIELDS",
    "COMMAND_OUTCOMES",
    "COMMAND_REPEAT_WINDOW_NS",
    "CONTROL_COMMAND_NAMES",
    "CommandHistory",
    "IO_OUTPUT_OUTCOMES",
    "MAXIMUM_COUNTED_REFUSALS",
    "STOP_ROUTE_COMMANDS",
    "PROGRAM_NODE_ATTEMPT_ERROR_OUTCOMES",
    "PROGRAM_NODE_ATTEMPT_FIELDS",
    "PROGRAM_NODE_ATTEMPT_OUTCOMES",
    "PROGRAM_RESET_FIELDS",
    "PROGRAM_RESET_OUTCOMES",
    "SCENE_CHANGE_FIELDS",
    "SCENE_CHANGE_OUTCOMES",
    "SCHEMA_VERSION",
    "apply_record",
    "bind_request_caller",
    "caller",
    "command_finished_record",
    "component_action_record",
    "io_output_record",
    "program_node_attempt_record",
    "program_reset_record",
    "record_activity",
    "runner_reply_outcome",
    "scene_change_record",
    "unbind_request_caller",
    "with_caller",
]
