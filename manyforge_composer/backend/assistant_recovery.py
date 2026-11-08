"""Wire-neutral recovery guidance for assistant clients.

This module deliberately knows nothing about the former Composer tool IDs,
node-catalog wire, or free-form backend error strings.  It contains only pure
presentation mechanics that a read client and a future operation client
may bind to typed owner-issued error contracts.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Any, Mapping, Sequence


NUDGES: tuple[str, ...] = (
    "Re-read the original request and the current authoritative context; "
    "verify the operation contract before retrying.",
    "Re-examine the requested outcome and the available typed operations; "
    "a different operation or smaller change may be required.",
    "Review the latest state and the rejected operation together, then retry "
    "only with a contract-valid request.",
    "Step back and check the requested outcome against the current "
    "capabilities before choosing the next operation.",
    "Confirm the operation's typed inputs and current preconditions; switch "
    "approaches if they do not match the request.",
    "Re-evaluate from the original request and choose the smallest operation "
    "whose contract can satisfy it.",
)

ATTACH_SNAPSHOT_AFTER_FAILURES = 3
IMPASSE_FAILURE_THRESHOLD = 15
META_NUDGE_AFTER_FAILURES = 5
FORMAT_STUCK_REPEAT_THRESHOLD = 3

_PARSE_COLUMN_RE = re.compile(r"line\s+\d+\s+column\s+(\d+)")


@dataclass(frozen=True, slots=True)
class RecoveryAttempt:
    """One client-local operation result used only for recovery guidance."""

    operation: str
    failed: bool
    error_class: str | None = None
    detail: Any = None


@dataclass(frozen=True, slots=True)
class RecoveryEnvelope:
    """Transport-neutral recovery clauses surrounding one diagnostic."""

    detail: Any
    action: str | None
    nudge: str
    extra_lines: tuple[str, ...] = ()

    def rendered_message(self) -> str | None:
        """Render a human/model-readable message without choosing a wire shape."""

        diagnostic = _diagnostic_text(self.detail)
        if diagnostic is None:
            return None
        parts = [diagnostic.rstrip(". ")]
        if self.action:
            parts.append(f"Action: {self.action.rstrip('. ')}")
        if self.nudge:
            parts.append(self.nudge.rstrip(". "))
        parts.extend(line.rstrip(". ") for line in self.extra_lines if line)
        return ". ".join(parts) + "."


def nudge_for_index(index: int) -> str:
    """Return a deterministic rotating nudge for a one-based failure index."""

    return NUDGES[(max(0, index - 1)) % len(NUDGES)]


def compose_recovery(
    detail: Any,
    action: str | None,
    nudge: str,
    *,
    extra_lines: Sequence[str] = (),
) -> RecoveryEnvelope:
    """Create an immutable envelope without mutating or serializing ``detail``."""

    return RecoveryEnvelope(
        detail=detail,
        action=action,
        nudge=nudge,
        extra_lines=tuple(line for line in extra_lines if line),
    )


def _diagnostic_text(detail: Any) -> str | None:
    if isinstance(detail, str):
        return detail
    if isinstance(detail, Mapping):
        message = detail.get("message")
        return message if isinstance(message, str) else None
    return None


def _extract_parse_column(detail: Any) -> int | None:
    text = _diagnostic_text(detail)
    if text is None:
        return None
    match = _PARSE_COLUMN_RE.search(text)
    if match is None:
        return None
    try:
        return int(match.group(1))
    except ValueError:
        return None


def truncation_pattern_nudge(
    history: Sequence[RecoveryAttempt],
) -> str | None:
    """Detect repeated parse failures at one byte column."""

    if len(history) < FORMAT_STUCK_REPEAT_THRESHOLD:
        return None
    recent = history[-FORMAT_STUCK_REPEAT_THRESHOLD:]
    if any(not attempt.failed for attempt in recent):
        return None
    columns = [_extract_parse_column(attempt.detail) for attempt in recent]
    if any(column is None for column in columns) or len(set(columns)) != 1:
        return None
    return (
        "The request has failed at the same parse offset on every retry. "
        "Stop resending the same large representation; use a smaller typed "
        "operation or split the change into bounded steps."
    )


def format_stuck_nudge(history: Sequence[RecoveryAttempt]) -> str | None:
    """Detect repeated failures for one operation and typed error class."""

    if len(history) < FORMAT_STUCK_REPEAT_THRESHOLD:
        return None
    recent = history[-FORMAT_STUCK_REPEAT_THRESHOLD:]
    if any(not attempt.failed for attempt in recent):
        return None
    operations = {attempt.operation for attempt in recent}
    error_classes = {attempt.error_class for attempt in recent}
    if len(operations) != 1 or len(error_classes) != 1:
        return None
    error_class = next(iter(error_classes))
    if not error_class:
        return None
    operation = next(iter(operations)) or "this operation"
    return (
        f"The same operation ({operation}) has produced {error_class} on "
        "every recent retry. Switch operations or restructure the request "
        "before trying again."
    )


def meta_nudge(
    history: Sequence[RecoveryAttempt],
    *,
    threshold: int,
) -> str | None:
    """Summarize a sustained client-local failure pattern."""

    failed = [attempt for attempt in history if attempt.failed]
    if len(failed) < threshold:
        return None
    operation_counts: dict[str, int] = {}
    error_counts: dict[str, int] = {}
    for attempt in failed:
        operation = attempt.operation or "unknown"
        operation_counts[operation] = operation_counts.get(operation, 0) + 1
        if attempt.error_class:
            error_counts[attempt.error_class] = (
                error_counts.get(attempt.error_class, 0) + 1
            )
    operation_summary = _count_summary(operation_counts)
    error_summary = _count_summary(error_counts) or "uncategorized"
    return (
        f"There have been {len(failed)} failed operations "
        f"(operations: {operation_summary}; classes: {error_summary}). "
        "Review the requested outcome and choose a smaller or different "
        "typed operation."
    )


def impasse_message(
    *,
    failure_count: int,
    history: Sequence[RecoveryAttempt],
) -> str:
    """Build the terminal guidance for an exhausted bounded client turn."""

    operation_counts: dict[str, int] = {}
    error_counts: dict[str, int] = {}
    for attempt in history:
        operation = attempt.operation or "unknown"
        operation_counts[operation] = operation_counts.get(operation, 0) + 1
        if attempt.error_class:
            error_counts[attempt.error_class] = (
                error_counts.get(attempt.error_class, 0) + 1
            )
    return (
        "Operation budget exhausted on this turn after "
        f"{failure_count} consecutive failed attempts. "
        f"Operations attempted: {_count_summary(operation_counts) or 'none'}. "
        f"Error classes encountered: {_count_summary(error_counts) or 'none'}. "
        "Stop issuing operations, explain the impasse, request direction, "
        "and propose a smaller bounded next step."
    )


def _count_summary(counts: Mapping[str, int]) -> str:
    return ", ".join(
        f"{name}×{count}"
        for name, count in sorted(
            counts.items(),
            key=lambda item: (-item[1], item[0]),
        )[:4]
    )


__all__ = [
    "ATTACH_SNAPSHOT_AFTER_FAILURES",
    "FORMAT_STUCK_REPEAT_THRESHOLD",
    "IMPASSE_FAILURE_THRESHOLD",
    "META_NUDGE_AFTER_FAILURES",
    "NUDGES",
    "RecoveryAttempt",
    "RecoveryEnvelope",
    "compose_recovery",
    "format_stuck_nudge",
    "impasse_message",
    "meta_nudge",
    "nudge_for_index",
    "truncation_pattern_nudge",
]
