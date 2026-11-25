"""History of scene changes from the Manager's status stream (OD-400).

Every scene change outside a Program node attempt, whoever made it (Composer,
the assistant, MCP, any HTTP caller, the Program runner, the execution host),
is recorded from one source: the Manager's ``scene_changes`` status section,
the last scene commit outcomes its command lane decided, each with a
``sequence`` rising by 1 in that Manager start. A Program node's change is in
its node attempt event; a replayed retry changed nothing; neither is in the
section.

The recorder runs on the status follower thread. Each value holds the whole
ring (bounded so it always fits one stream frame), so a value that was
skipped (coalesced, a reconnect) costs nothing while its records are still
held; records that left the ring before
Headless saw them are one ``records_lost`` event with their count. Recording
is a hand-off to the historian's bounded queue (memory only, never a wait): a
full queue is the historian's own lost-records event. After a Headless
restart, the last recorded sequence of the same Manager start is read once
from History, so no change is recorded twice.
"""

from __future__ import annotations

from collections.abc import Mapping
import logging
from typing import Any, Callable, Final

from .activity_history import SCHEMA_VERSION, record_activity, scene_change_stream_record

SCENE_CHANGE_PUBLISHER: Final = "manager_scene_changes"
SCENE_CHANGE_EVENT_TYPE: Final = "scene_change_finished"

_LOGGER = logging.getLogger(__name__)


def scene_changes_lost_record(count: int) -> dict[str, Any]:
    """Scene changes that left the Manager's ring before Headless saw them."""

    return {
        "schema_version": SCHEMA_VERSION,
        "kind": "records_lost",
        "subject": SCENE_CHANGE_PUBLISHER,
        "action": None,
        "outcome": "lost",
        "error": None,
        "components": [],
        "message": (
            f"Scene change history lost {count} "
            f"{'record' if count == 1 else 'records'}"
        ),
        "records_lost": {"lost_count": count},
    }


def last_recorded_scene_change(
    history_export: Callable[[], Mapping[str, Any]],
) -> tuple[str, int] | None:
    """The Manager start and sequence of the newest scene change History
    holds from the stream (read once at start), or None."""

    for stored in history_export().get("events", []):
        facts = stored.get("event", {}).get("payload", {}).get("scene_change", {})
        start, sequence = facts.get("manager_start_id"), facts.get("sequence")
        if isinstance(start, str) and type(sequence) is int:
            return start, sequence
    return None


class ManagerSceneChangeRecorder:
    """Records each new record of the ``scene_changes`` section once.
    Follower thread only."""

    def __init__(self, core: Any, *, last_recorded: tuple[str, int] | None) -> None:
        self._core = core
        self._last_recorded = last_recorded
        self._start: str | None = None
        self._last = 0

    def observe(self, changes: Mapping[str, Any]) -> None:
        """One usable value ``{application_start_id, records}``; never raises
        (a failure loses the History lines, never the stream)."""

        try:
            self._observe(changes["application_start_id"], changes["records"])
        except Exception:  # noqa: BLE001 - a lost History line never ends the stream
            _LOGGER.warning("scene_change_history_failed", exc_info=True)

    def _observe(self, start: str, records: list[Mapping[str, Any]]) -> None:
        if start != self._start:
            self._start = start
            self._last = (
                self._last_recorded[1]
                if self._last_recorded is not None and self._last_recorded[0] == start
                else 0
            )
        fresh = [record for record in records if record["sequence"] > self._last]
        if not fresh:
            return
        if (lost := fresh[0]["sequence"] - self._last - 1) > 0:
            record_activity(self._core, scene_changes_lost_record(lost))
        for record in fresh:
            record_activity(self._core, scene_change_stream_record(start, record))
        self._last = fresh[-1]["sequence"]


__all__ = [
    "SCENE_CHANGE_EVENT_TYPE",
    "SCENE_CHANGE_PUBLISHER",
    "ManagerSceneChangeRecorder",
    "last_recorded_scene_change",
    "scene_changes_lost_record",
]
