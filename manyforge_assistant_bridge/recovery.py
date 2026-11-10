"""Stuck-loop recovery for one assistant request (OD-76).

A model can repeat the same failing tool call many times. When the same tool
fails with the same error class a second time, the tool result carries a typed
``recovery`` object: what to change, plus a rotating step-back hint, so
consecutive failures do not read alike. On the fifth such failure the request
ends and asks the operator for clarification. The mode's
``stuck_loop_recovery`` setting turns both off.

Ported from the 0.1.0 Composer recovery helpers; the error class is the typed
failure code, never a match on message text.
"""

from __future__ import annotations

from dataclasses import dataclass, field
import json
from typing import Any, Final, Mapping

HINT_AFTER_REPEATS: Final = 2
CLARIFY_AFTER_REPEATS: Final = 5
RECOVERY_CODE: Final = "assistant_repeated_tool_failure"

# Equal-weight rephrasings of one instruction; consecutive failures get
# different text so the model does not settle into the same reply.
NUDGES: Final = (
    "Re-read the original request, the listed nodes and the catalog entry; then retry "
    "with a corrected payload or choose a different tool.",
    "Re-examine the request alongside catalog_read for the node before the next call; "
    "a different tool or a different shape may be needed.",
    "Read the Draft again with program_read, then either fix the payload or pick a tool "
    "whose contract matches the request more closely.",
    "Step back: review the request and the node's catalog entry before retrying; the "
    "right move may be a different tool.",
)

ACTION_BY_CLASS: Final = {
    "assistant_tool_arguments_invalid": (
        "Change the field named by path to the expected shape; call catalog_read with "
        "include_parameters for the node if the shape is unclear."
    ),
}
# OD-92: the same successful Draft edit repeated (same tool, same arguments).
REPEATED_EDIT_CLASS: Final = "repeated_identical_edit"
ACTION_BY_CLASS[REPEATED_EDIT_CLASS] = (
    "This exact edit already succeeded; do not repeat it. Read the Draft with "
    "program_read, then continue with the next step or answer."
)
DEFAULT_ACTION: Final = (
    "Do not repeat the same call: change the arguments using the diagnostic, or use "
    "a different tool."
)


def error_class(result: Mapping[str, Any]) -> str:
    """Typed class of one failed tool result: its detail code, else its kind."""

    payload = result.get("result")
    if isinstance(payload, dict):
        detail = payload.get("detail")
        if isinstance(detail, dict) and isinstance(detail.get("code"), str):
            return detail["code"]
        if isinstance(payload.get("kind"), str):
            return payload["kind"]
    return "unknown"


@dataclass
class StuckLoopTracker:
    """Counts same-tool, same-class failures within one request."""

    enabled: bool
    counts: dict[tuple[str, str, str], int] = field(default_factory=dict)

    def record(self, tool_id: str, result: Mapping[str, Any]) -> dict[str, Any] | None:
        """Count one failure; return the typed recovery object when due."""

        if not self.enabled:
            return None
        return self._count(tool_id, error_class(result))

    def record_success(self, tool_id: str, arguments: Mapping[str, Any]) -> dict[str, Any] | None:
        """Count one successful Draft edit; identical repeats get the same recovery."""

        if not self.enabled:
            return None
        identity = json.dumps(arguments, sort_keys=True, separators=(",", ":"))
        return self._count(tool_id, REPEATED_EDIT_CLASS, identity)

    def _count(self, tool_id: str, cls: str, identity: str = "") -> dict[str, Any] | None:
        key = (tool_id, cls, identity)
        repeats = self.counts.get(key, 0) + 1
        self.counts[key] = repeats
        if repeats < HINT_AFTER_REPEATS:
            return None
        return {
            "code": RECOVERY_CODE,
            "toolId": tool_id,
            "errorClass": cls,
            "repeats": repeats,
            "action": ACTION_BY_CLASS.get(cls, DEFAULT_ACTION),
            "hint": NUDGES[(repeats - HINT_AFTER_REPEATS) % len(NUDGES)],
            "clarificationRequired": repeats >= CLARIFY_AFTER_REPEATS,
        }

    def clarification_message(self, recovery: Mapping[str, Any]) -> str:
        attempted = ", ".join(
            f"{tool} ({cls}) x{count}"
            for (tool, cls, _identity), count in sorted(self.counts.items(), key=lambda row: -row[1])
        )
        what = ("repeated the same edit" if recovery["errorClass"] == REPEATED_EDIT_CLASS
                else f"failed with {recovery['errorClass']}")
        return (
            f"I need clarification: {recovery['toolId']} {what} {recovery['repeats']} "
            f"times. Attempts: {attempted}. Please say how to proceed, or give a simpler "
            "request."
        )


__all__ = [
    "CLARIFY_AFTER_REPEATS",
    "HINT_AFTER_REPEATS",
    "RECOVERY_CODE",
    "StuckLoopTracker",
    "error_class",
]
