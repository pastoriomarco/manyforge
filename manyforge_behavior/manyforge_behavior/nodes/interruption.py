"""The `interrupted` outcome of a cancellable leaf.

A catalog leaf with `execution.cancellable: true` declares the `interrupted`
outcome; a non-cancellable leaf never does. A Program Stop,
a Reset, a cancel or a parent that ends the node stops a running attempt of
a cancellable leaf with that outcome and a reason code, never with
`failure`. py_trees has no third terminal status: an interrupted attempt
keeps the INVALID status the tree's own stop leaves, and this record says
why. Its next attempt starts clean. A Halt freezes the run without stopping
the tree, so it interrupts nothing: Execute continues the same attempt. An
operator Stop that holds a node is not an interruption until the held
attempt is stopped.

A stop is an interruption only once it is confirmed. When the owner refuses
to cancel the running work, the stop is unconfirmed: the attempt reports
`failure` (the adapter records `owner_cancellation_failed`) so the run pauses
for operator attention instead of presenting the work as interrupted.
"""

from __future__ import annotations

from typing import Final

import py_trees

NODE_INTERRUPTED_REASON: Final = "program_node_interrupted"


class InterruptedOutcome:
    """Record the interruption of one attempt of a cancellable py_trees leaf."""

    cancellable: bool = True
    _interrupted_reason_code: str | None = None
    _interrupted_message: str = ""
    _stop_failed: bool = False

    def _clear_interruption(self) -> None:
        self._interrupted_reason_code = None
        self._interrupted_message = ""
        self._stop_failed = False

    def _record_interruption(
        self,
        new_status: py_trees.common.Status,
        *,
        reason_code: str = NODE_INTERRUPTED_REASON,
        message: str = "the running attempt was stopped",
    ) -> bool:
        """Record when `terminate(new_status)` ends a running attempt."""

        if (
            not self.cancellable
            or new_status is not py_trees.common.Status.INVALID
            or self.status is not py_trees.common.Status.RUNNING  # type: ignore[attr-defined]
        ):
            return False
        self._interrupted_reason_code = reason_code
        self._interrupted_message = message
        return True

    def _record_stop_failure(self) -> None:
        """The stop of this attempt was not confirmed: report `failure`.

        The caller records the failure reason; this only replaces the
        interruption the stop recorded.
        """

        self._interrupted_reason_code = None
        self._interrupted_message = ""
        self._stop_failed = True

    @property
    def interrupted_reason_code(self) -> str | None:
        return self._interrupted_reason_code

    @property
    def interrupted_message(self) -> str:
        return self._interrupted_message

    @property
    def outcome(self) -> str | None:
        """The catalog outcome of the last attempt, or None while it runs."""

        status = self.status  # type: ignore[attr-defined]
        if status is py_trees.common.Status.SUCCESS:
            return "success"
        if status is py_trees.common.Status.FAILURE:
            return "failure"
        if status is py_trees.common.Status.INVALID and self._stop_failed:
            return "failure"
        if (
            status is py_trees.common.Status.INVALID
            and self._interrupted_reason_code is not None
        ):
            return "interrupted"
        return None

    @property
    def presented_interrupted(self) -> bool:
        return self.outcome == "interrupted"

    @property
    def presented_stop_failed(self) -> bool:
        """A stopped attempt whose stop was not confirmed reports `failure`."""

        return self._stop_failed and self.outcome == "failure"


__all__ = ["InterruptedOutcome", "NODE_INTERRUPTED_REASON"]
