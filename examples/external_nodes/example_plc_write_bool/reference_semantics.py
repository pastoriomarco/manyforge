"""Pure acceptance reducer for the non-operative PLC-write example.

This module performs no I/O, imports no orchestration engine, resolves no
implementation or provider, and owns no command or Stop truth.  It retains only
the dependency-neutral attempt decisions from the historical example so the
future I6/N owner client has an exact behavioral reference.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import Final


MAX_ATTEMPT_TIMEOUT_NS: Final = 10_000_000_000
MAX_DIAGNOSTIC_CHARACTERS: Final = 256


class WriteBoolPhase(StrEnum):
    READY = "ready"
    RUNNING = "running"
    SUCCEEDED = "succeeded"
    FAILED = "failed"
    CANCELLED = "cancelled"


class WriteBoolEffect(StrEnum):
    SUBMIT_COMMAND = "submit_command"
    CANCEL_COMMAND = "cancel_command"


_TERMINAL_PHASES: Final = frozenset(
    {
        WriteBoolPhase.SUCCEEDED,
        WriteBoolPhase.FAILED,
        WriteBoolPhase.CANCELLED,
    }
)


def _bounded_ns(value: object, *, label: str, positive: bool = False) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} must be an integer")
    minimum = 1 if positive else 0
    if value < minimum:
        relation = "> 0" if positive else ">= 0"
        raise ValueError(f"{label} must be {relation}")
    return value


def _bounded_diagnostic(value: str | None, *, fallback: str) -> str:
    if value is None:
        return fallback
    if not isinstance(value, str):
        raise ValueError("result_error must be a string or null")
    printable = "".join(
        character if character.isprintable() else " " for character in value
    )
    normalized = " ".join(printable.split())
    if not normalized:
        return fallback
    return normalized[:MAX_DIAGNOSTIC_CHARACTERS]


@dataclass(frozen=True, slots=True)
class WriteBoolAttempt:
    phase: WriteBoolPhase
    timeout_ns: int
    started_ns: int | None = None
    diagnostic: str = ""

    def __post_init__(self) -> None:
        if not isinstance(self.phase, WriteBoolPhase):
            raise ValueError("phase must be a WriteBoolPhase")
        timeout_ns = _bounded_ns(
            self.timeout_ns,
            label="timeout_ns",
            positive=True,
        )
        if timeout_ns > MAX_ATTEMPT_TIMEOUT_NS:
            raise ValueError("timeout_ns exceeds the 10-second example bound")
        if self.started_ns is not None:
            _bounded_ns(self.started_ns, label="started_ns")
        if self.phase is WriteBoolPhase.READY and self.started_ns is not None:
            raise ValueError("a ready attempt cannot have started_ns")
        if (
            self.phase
            in {
                WriteBoolPhase.RUNNING,
                WriteBoolPhase.SUCCEEDED,
                WriteBoolPhase.FAILED,
            }
            and self.started_ns is None
        ):
            raise ValueError("a started attempt requires started_ns")
        if not isinstance(self.diagnostic, str):
            raise ValueError("diagnostic must be a string")
        if len(self.diagnostic) > MAX_DIAGNOSTIC_CHARACTERS:
            raise ValueError("diagnostic exceeds the 256-character bound")


@dataclass(frozen=True, slots=True)
class WriteBoolDecision:
    attempt: WriteBoolAttempt
    effects: tuple[WriteBoolEffect, ...] = ()


def new_write_bool_attempt(*, timeout_ns: int) -> WriteBoolAttempt:
    """Create one validated attempt without starting a command."""

    return WriteBoolAttempt(
        phase=WriteBoolPhase.READY,
        timeout_ns=timeout_ns,
    )


def reduce_write_bool_tick(
    attempt: WriteBoolAttempt,
    *,
    now_ns: int,
    operation_done: bool = False,
    result_ok: bool | None = None,
    result_error: str | None = None,
) -> WriteBoolDecision:
    """Reduce one observation into a state and requested owner-client effect."""

    now_ns = _bounded_ns(now_ns, label="now_ns")
    if not isinstance(operation_done, bool):
        raise ValueError("operation_done must be a boolean")
    if attempt.phase in _TERMINAL_PHASES:
        return WriteBoolDecision(attempt=attempt)

    if attempt.phase is WriteBoolPhase.READY:
        if operation_done or result_ok is not None or result_error is not None:
            raise ValueError("a ready attempt cannot carry an operation result")
        return WriteBoolDecision(
            attempt=WriteBoolAttempt(
                phase=WriteBoolPhase.RUNNING,
                timeout_ns=attempt.timeout_ns,
                started_ns=now_ns,
            ),
            effects=(WriteBoolEffect.SUBMIT_COMMAND,),
        )

    assert attempt.started_ns is not None
    if now_ns < attempt.started_ns:
        raise ValueError("now_ns must not precede started_ns")
    if now_ns - attempt.started_ns >= attempt.timeout_ns:
        return WriteBoolDecision(
            attempt=WriteBoolAttempt(
                phase=WriteBoolPhase.FAILED,
                timeout_ns=attempt.timeout_ns,
                started_ns=attempt.started_ns,
                diagnostic="command_timeout",
            ),
            effects=() if operation_done else (WriteBoolEffect.CANCEL_COMMAND,),
        )

    if not operation_done:
        if result_ok is not None or result_error is not None:
            raise ValueError("an incomplete operation cannot carry a result")
        return WriteBoolDecision(attempt=attempt)
    if not isinstance(result_ok, bool):
        raise ValueError("a completed operation requires boolean result_ok")
    if result_ok:
        return WriteBoolDecision(
            attempt=WriteBoolAttempt(
                phase=WriteBoolPhase.SUCCEEDED,
                timeout_ns=attempt.timeout_ns,
                started_ns=attempt.started_ns,
            )
        )
    return WriteBoolDecision(
        attempt=WriteBoolAttempt(
            phase=WriteBoolPhase.FAILED,
            timeout_ns=attempt.timeout_ns,
            started_ns=attempt.started_ns,
            diagnostic=_bounded_diagnostic(
                result_error,
                fallback="command_failed",
            ),
        )
    )


def reduce_write_bool_cancellation(
    attempt: WriteBoolAttempt,
) -> WriteBoolDecision:
    """Request cancellation only when this reference attempt is in flight."""

    if attempt.phase in _TERMINAL_PHASES:
        return WriteBoolDecision(attempt=attempt)
    return WriteBoolDecision(
        attempt=WriteBoolAttempt(
            phase=WriteBoolPhase.CANCELLED,
            timeout_ns=attempt.timeout_ns,
            started_ns=attempt.started_ns,
            diagnostic="command_cancelled",
        ),
        effects=(
            (WriteBoolEffect.CANCEL_COMMAND,)
            if attempt.phase is WriteBoolPhase.RUNNING
            else ()
        ),
    )


__all__ = [
    "MAX_ATTEMPT_TIMEOUT_NS",
    "MAX_DIAGNOSTIC_CHARACTERS",
    "WriteBoolAttempt",
    "WriteBoolDecision",
    "WriteBoolEffect",
    "WriteBoolPhase",
    "new_write_bool_attempt",
    "reduce_write_bool_cancellation",
    "reduce_write_bool_tick",
]
