"""Command rows of the commands answered at acceptance (OD-214).

A command row describes one command from the moment it holds its owner's slot
until its end: ``kind``, ``request_key``, ``step``, ``accepted_at_ns`` and
``end`` (``outcome``, typed ``code``, the error's ``details``,
``finished_at_ns``); times are wall-clock ns as decimal text, as the runner's
(``program_cycle.RunnerCommandRowDTO``). The Program runner publishes its own rows; this table is
the same shape for the commands Headless (activation preparation) and the
Composer (Apply, Load, New, ...) run themselves. It holds the command in
progress and the last ended rows, newest first, in memory only.

The table decides nothing: its owner holds the command's slot (one command at
a time), and calls ``accept``, ``step`` and ``end``. The lock is a leaf lock;
the change observer is called after it is released.
"""

from __future__ import annotations

from collections import deque
from copy import deepcopy
import threading
import time
from typing import Any, Callable, Mapping

from .program_cycle import MAXIMUM_ENDED_COMMAND_ROWS, CommandOutcome


class CommandRowTable:
    """The command in progress and the last ``MAXIMUM_ENDED_COMMAND_ROWS`` ended."""

    def __init__(
        self,
        *,
        on_change: Callable[[], None] | None = None,
        clock_ns: Callable[[], int] = time.time_ns,
    ) -> None:
        self._lock = threading.Lock()
        self._in_progress: dict[str, Any] | None = None
        self._ended: deque[dict[str, Any]] = deque(maxlen=MAXIMUM_ENDED_COMMAND_ROWS)
        self._on_change = on_change
        self._clock_ns = clock_ns

    def set_observer(self, on_change: Callable[[], None] | None) -> None:
        self._on_change = on_change

    def _changed(self) -> None:
        if self._on_change is not None:
            self._on_change()

    def find(self, request_key: str) -> dict[str, Any] | None:
        """The kept row with this request key, as it now stands, or ``None``."""

        with self._lock:
            if self._in_progress is not None and self._in_progress["request_key"] == request_key:
                return deepcopy(self._in_progress)
            for row in self._ended:
                if row["request_key"] == request_key:
                    return deepcopy(row)
        return None

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "in_progress": deepcopy(self._in_progress),
                "ended": deepcopy(list(self._ended)),
            }

    def accept(
        self,
        kind: str,
        request_key: str,
        *,
        step: str | None = None,
        **members: Any,
    ) -> dict[str, Any]:
        """The accepted command's row in progress; its owner holds the slot."""

        row = {
            "kind": kind,
            "request_key": request_key,
            "step": step,
            "accepted_at_ns": str(self._clock_ns()),
            "end": None,
            **members,
        }
        with self._lock:
            if self._in_progress is not None:
                raise RuntimeError("a command row is already in progress")
            self._in_progress = row
            reply = deepcopy(row)
        self._changed()
        return reply

    def step(self, request_key: str, step: str, **members: Any) -> None:
        """The command in progress reached ``step`` (and its row's members)."""

        with self._lock:
            row = self._in_progress
            if row is None or row["request_key"] != request_key:
                raise RuntimeError("the command row is not in progress")
            row["step"] = step
            row.update(members)
        self._changed()

    def update(self, request_key: str, **members: Any) -> None:
        """Change members of the command in progress (its step stays)."""

        with self._lock:
            row = self._in_progress
            if row is None or row["request_key"] != request_key:
                raise RuntimeError("the command row is not in progress")
            row.update(members)
        self._changed()

    def end(
        self,
        request_key: str,
        outcome: CommandOutcome,
        code: str | None = None,
        details: Mapping[str, Any] | None = None,
    ) -> dict[str, Any]:
        """End the command in progress; its row moves to the ended rows."""

        with self._lock:
            row = self._in_progress
            if row is None or row["request_key"] != request_key:
                raise RuntimeError("the command row is not in progress")
            row["end"] = _end(outcome, code, details, self._clock_ns())
            self._in_progress = None
            self._ended.appendleft(row)
            ended = deepcopy(row)
        self._changed()
        return ended

    def refuse(
        self,
        kind: str,
        request_key: str,
        code: str,
        details: Mapping[str, Any] | None = None,
        *,
        outcome: CommandOutcome = "refused",
        **members: Any,
    ) -> dict[str, Any]:
        """An end before acceptance: an ended row, never accepted. A typed
        refusal is ``refused``; an untyped failure ``unknown``."""

        row = {
            "kind": kind,
            "request_key": request_key,
            "step": None,
            "accepted_at_ns": None,
            "end": _end(outcome, code, details, self._clock_ns()),
            **members,
        }
        with self._lock:
            self._ended.appendleft(row)
            ended = deepcopy(row)
        self._changed()
        return ended


def _end(
    outcome: CommandOutcome,
    code: str | None,
    details: Mapping[str, Any] | None,
    finished_at_ns: int,
) -> dict[str, Any]:
    if (outcome == "succeeded") != (code is None):
        raise ValueError("a command end has a code exactly when it did not succeed")
    return {
        "outcome": outcome,
        "code": code,
        # A succeeded Apply-level row keeps its component rows (§3.1).
        "details": deepcopy(dict(details)) if details else None,
        "finished_at_ns": str(finished_at_ns),
    }


__all__ = ["CommandRowTable"]
