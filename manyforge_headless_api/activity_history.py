"""History records of component actions, Apply and Program Reset terminals,
Program node attempts and scene changes.

One event per finished component action (whoever asked for it), one per
Scenario Load / New, Deployment Apply / Revert and Program Apply / Revert
terminal, one per Program Reset terminal (OD-305), one per finished Program
node attempt and one per scene change made outside a Program node
(OD-375, OD-378). Each record carries the typed outcome and error code;
``message`` is the History list's display line, built here from those typed
fields. The native historian validates the record and stores it in the
existing bounded History (no store of its own).
"""

from __future__ import annotations

from collections.abc import Mapping
import logging
from typing import Any, Final

import rfc8785

from manyforge_behavior.failure_details import MAX_FAILURE_DETAILS_BYTES

SCHEMA_VERSION: Final = "manyforge.activity_event.v1"
APPLY_KINDS: Final = frozenset({
    "scenario_load", "scenario_new", "deployment_apply", "deployment_revert",
    "program_apply", "program_revert",
})
PROGRAM_RESET_OUTCOMES: Final = frozenset({"succeeded", "refused", "failed", "unknown"})
PROGRAM_RESET_FIELDS: Final = (
    "reload_initial_scene", "reset_robot_position", "program_run_id",
    "program_revision", "applied_record_digest", "scene_revision_id",
    "scene_content_hash", "cause_code",
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
_ERROR_FIELDS: Final = ("code", "message", "vendor_code", "vendor_text")
_LOGGER = logging.getLogger(__name__)


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
    "PROGRAM_NODE_ATTEMPT_ERROR_OUTCOMES",
    "PROGRAM_NODE_ATTEMPT_FIELDS",
    "PROGRAM_NODE_ATTEMPT_OUTCOMES",
    "PROGRAM_RESET_FIELDS",
    "PROGRAM_RESET_OUTCOMES",
    "SCENE_CHANGE_FIELDS",
    "SCENE_CHANGE_OUTCOMES",
    "SCHEMA_VERSION",
    "apply_record",
    "component_action_record",
    "program_node_attempt_record",
    "program_reset_record",
    "record_activity",
    "scene_change_record",
]
