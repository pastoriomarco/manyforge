"""Always-current mirror of the Kernel's global interrupt rule states.

The stream's ``state`` (and ``snapshot``) and the ``events`` reply carry the
outer ``interrupt_rule_state_baseline``, built from Kernel memory: null (no
installed global rule catalog), ``{"status": "projection_pending"}`` (a rule
waits for a Program, so no state is known) or
``{"status": "ready", "snapshot": ...}`` with every rule's current state.
The mirror takes the states of a ready baseline whose context the same
message's catalog proves; anything else leaves them unavailable, never clear.
Journal rule rows are not read: a trip that starts and ends between two
``state`` messages never shows here. An unreadable reply or a malformed
message drops the mirror until the next ready baseline.

The same reply carries ``interrupt_rule_catalog``, independent of the baseline's readiness: null (unknown or pending,
never empty), ``{"status": "verified_empty",
active_publication_generation, bundle, graph}`` (proof of no global rule for
that context) or the installed catalog's context with its sorted Applied
global ``rule_ids`` and the sorted union of their checked condition BB operands
(``referenced_program_blackboard_keys``), optionally with the per-rule
``waiting_for_program`` annotation, or ``{"status": "waiting_for_program",
active_publication_generation, bundle, graph, rule_ids,
waiting_for_program}`` (no Program declares a key of these
rules, so no rule catalog is installed and no state is sent). The mirror keeps
the latest one read; its states are current only while that catalog proves
their full context, and a waiting rule's state is unavailable, never clear.

Delivery is a streaming subscription (``follow``): the host sends the
reply's non-page facts as ``state`` when they change, after a first
``snapshot``; every ready baseline in them is installed. A dead link makes
the states unavailable until the next snapshot. ``poll`` stays the one-shot
read for a caller that needs a fresh catalog at once; with a subscription it
installs only a baseline of a context the mirror does not have yet, since a
later ``state`` carries every change after it.

The same ``state`` (and ``snapshot``) carries ``program_run_hold``: the
Kernel's in-memory hold state of the claimed run (null, or run, hold
revision, frozen level and the event that froze it, all-groups flag, holding
rule), and the cell-wide markers ``last_stop_event_id`` and
``stop_progress_sequence``. The mirror parses them into one
``ProgramRunHoldFacts`` and hands each change, after its lock is released, to
``on_program_run_hold`` on the follower thread.
"""

from __future__ import annotations

from collections.abc import Callable, Mapping, Sequence
import logging
from threading import Event, Lock
from typing import Any, Final

from manyforge_behavior.program_scheduling import ProgramRunHoldFacts
from manyforge_headless_api.execution_event_stream import follow_execution_events

# The wire parsers are shared with the Headless GUI projection.
from manyforge_headless_api.global_rule_status import (
    RULE_STATES,
    InterruptStateWireError,
    RuleCatalog,
    RuleStateBaseline,
    RuleStateContext,
    catalog_proves as _proves,
    parse_baseline,
    parse_catalog,
)

_MAX_SAFE: Final = 9_007_199_254_740_991

_LOGGER = logging.getLogger(__name__)


def _program_run_hold(state: Mapping[str, Any]) -> ProgramRunHoldFacts | None:
    """``state.program_run_hold`` with the state's Stop markers (event
    stream v2); null without a claimed run; malformed raises."""

    hold = state["program_run_hold"]
    if hold is None:
        return None
    try:
        return ProgramRunHoldFacts.from_wire(
            hold,
            last_stop_event_id=state["last_stop_event_id"],
            stop_progress_sequence=state["stop_progress_sequence"],
        )
    except (KeyError, ValueError) as error:
        raise InterruptStateWireError(str(error)) from error


def _page(response: Mapping[str, Any], after: int) -> Mapping[str, Any]:
    page = response.get("event_page")
    if (
        response.get("command") != "events"
        or not isinstance(page, Mapping)
        or type(page.get("next_after_sequence")) is not int
        or not after <= page["next_after_sequence"] <= _MAX_SAFE
        or type(page.get("has_more")) is not bool
        or not isinstance(page.get("events"), list)
    ):
        raise InterruptStateWireError("events reply has an invalid page envelope")
    previous = after
    for event in page["events"]:
        if (
            not isinstance(event, Mapping)
            or type(event.get("sequence")) is not int
            or not previous < event["sequence"] <= page["next_after_sequence"]
        ):
            raise InterruptStateWireError("events reply page is not ordered")
        previous = event["sequence"]
    return page


_LOSS_ROW_KEYS: Final = frozenset({
    "carried_object_attachment_id",
    "command_target_id",
    "controllable_component_id",
    "lost",
    "loss_event_id",
    "scene_resource_id",
    "set_key",
    "signal_name",
})
_LOSS_ROW_TEXT_KEYS: Final = _LOSS_ROW_KEYS - {"lost", "set_key"}


def _carried_object_lost_rows(rows: Any) -> tuple[Mapping[str, Any], ...] | None:
    """The carried-object-lost rows, strictly: closed keys, non-empty strings (``set_key``
    a non-empty string or null), ``lost: true`` and the target's signal name.
    Any invalid row makes the whole set unknown (None, nothing offered); the
    rule states are unaffected."""

    if not isinstance(rows, list):
        _LOGGER.warning("carried_object_lost_signals is not a list")
        return None
    for row in rows:
        if (
            not isinstance(row, Mapping)
            or set(row) != _LOSS_ROW_KEYS
            or row["lost"] is not True
            or any(type(row[key]) is not str or not row[key] for key in _LOSS_ROW_TEXT_KEYS)
            or not (row["set_key"] is None or (type(row["set_key"]) is str and row["set_key"]))
            or row["signal_name"] != f"carried_object_lost.{row['command_target_id']}"
        ):
            _LOGGER.warning("carried_object_lost_signals has an invalid row")
            return None
    return tuple(rows)


_LAST_LOSS_KEYS: Final = frozenset({
    "command_target_id",
    "controllable_component_id",
    "loss_event_id",
    "set_key",
})


def _carried_object_last_losses(entries: Any) -> tuple[Mapping[str, Any], ...] | None:
    """The last losses, strictly: closed keys, all non-empty strings, one
    entry per (robot, target, key). Any invalid entry makes the set unknown."""

    if not isinstance(entries, list):
        _LOGGER.warning("carried_object_last_losses is not a list")
        return None
    seen: set[tuple[str, str, str]] = set()
    for entry in entries:
        if (
            not isinstance(entry, Mapping)
            or set(entry) != _LAST_LOSS_KEYS
            or any(type(entry[key]) is not str or not entry[key] for key in _LAST_LOSS_KEYS)
            or (identity := (entry["controllable_component_id"], entry["command_target_id"],
                             entry["set_key"])) in seen
        ):
            _LOGGER.warning("carried_object_last_losses has an invalid entry")
            return None
        seen.add(identity)
    return tuple(entries)


class InterruptRuleStateMirror:
    """Follow the Kernel's global rule states through the stream's ``state``.

    ``read_events(after_sequence)`` is the existing events
    command; ``on_change(states)`` receives ``None`` (unavailable) or the
    complete current rule_id → state mapping whenever that changes.
    """

    def __init__(
        self,
        *,
        read_events: Callable[[int], Mapping[str, Any]],
        on_change: Callable[[Mapping[str, str] | None], None],
        subscribe: Callable[[int], Any] | None = None,
        on_carried_object_lost: Callable[[tuple[Mapping[str, Any], ...]], None] | None = None,
        on_program_run_hold: Callable[[ProgramRunHoldFacts], None] | None = None,
    ) -> None:
        self._read_events = read_events
        self._on_change = on_change
        # The event subscription (``follow``) and the latest
        # ``carried_object_lost_signals`` rows read, given to their consumer.
        self._subscribe = subscribe
        self._on_carried_object_lost = on_carried_object_lost
        self._on_program_run_hold = on_program_run_hold
        # The last program-run hold facts handed out (False: none yet).
        self._program_run_hold: ProgramRunHoldFacts | None | bool = False
        # None: unknown (no snapshot yet, a lost stream): nothing is
        # offered then, never an inferred empty set.
        self._carried_object_lost: tuple[Mapping[str, Any], ...] | None = None
        # Coalescing: the last loss per gripper and key; known
        # and unknown together with the rows.
        self._carried_object_last_losses: tuple[Mapping[str, Any], ...] | None = None
        self._lock = Lock()
        self._context: RuleStateContext | None = None
        # The journal position a poll reads its page after.
        self._cursor = 0
        self._states: dict[str, str] = {}
        self._published: Mapping[str, str] | None | bool = False
        # The latest installed global catalog read (None: none installed or
        # unreadable); `catalog_read` is False until a reply was read.
        self._catalog: RuleCatalog | None = None
        self._catalog_read = False

    def current(
        self, context: tuple[int, str, str] | None = None
    ) -> Mapping[str, str] | None:
        """The current states, or None (unavailable). Given a full context
        (publication, bundle, graph), None unless the mirror's
        states are of exactly that context."""

        with self._lock:
            states = self._current_locked()
            if states is None or context is None:
                return states
            own = self._context
            assert own is not None
            if context != (
                own.active_publication_generation,
                own.retained_bundle_id, own.graph_revision,
            ):
                return None
            return states

    def _current_locked(self) -> dict[str, str] | None:
        # Current only while the latest catalog proves exactly their context:
        # an unknown (None) catalog proves none. A rule waiting for
        # a Program is unavailable, never clear.
        context = self._context
        if context is None or self._catalog is None or not _proves(self._catalog, context):
            return None
        waiting = self._catalog.waiting_rule_ids
        return {rule_id: state for rule_id, state in self._states.items()
                if rule_id not in waiting and state != "unknown"}

    def catalog(self) -> RuleCatalog | None:
        """The installed global rule catalog, polling once if none was read."""

        if not self._catalog_read:
            self.poll()
        with self._lock:
            return self._catalog

    def _drop(self) -> None:
        self._context = None
        self._states = {}

    def poll(self) -> None:
        """One bounded read on the existing cadence (never concurrent)."""

        if not self._lock.acquire(blocking=False):
            return
        try:
            self._poll_locked()
            current, changed = self._publish_locked()
        finally:
            self._lock.release()
        if changed:
            self._on_change(current)

    def _publish_locked(self) -> tuple[Mapping[str, str] | None, bool]:
        current = self._current_locked()
        changed = current != self._published
        self._published = current
        return current, changed

    # Event subscription ---------------------------------------------------------

    def follow(
        self, stop: Event, *, on_read: Callable[[], None] | None = None,
        on_message: Callable[[Mapping[str, Any]], None] | None = None,
        on_disconnect: Callable[[BaseException], None] | None = None,
    ) -> None:
        """Apply the event subscription until ``stop`` (reconnecting from the
        last cursor, dead link after three missed heartbeats). ``on_read`` runs
        after every message but a row or progress: a reading, also when
        unchanged."""

        if self._subscribe is None:
            return

        def apply_message(message: Mapping[str, Any]) -> None:
            self.apply_stream_message(message)
            if on_message is not None:
                on_message(message)
            if message["type"] not in ("event", "progress") and on_read is not None:
                on_read()

        def stream_lost(error: BaseException) -> None:
            self._stream_lost(error)
            if on_disconnect is not None:
                on_disconnect(error)

        follow_execution_events(
            self._subscribe, apply_message, stop, on_disconnect=stream_lost
        )

    def apply_stream_message(self, message: Mapping[str, Any]) -> None:
        """Apply one validated stream message. A malformed one makes the
        states unavailable and raises (the subscription reconnects)."""

        kind = message["type"]
        rows: Sequence[Any] | None = None
        failure: Exception | None = None
        hold_changed = False
        hold: ProgramRunHoldFacts | None = None
        with self._lock:
            try:
                if kind in ("snapshot", "state"):
                    hold = _program_run_hold(message["state"])
                    self._apply_stream_state_locked(message["state"])
                    hold_changed = hold != self._program_run_hold
                    self._program_run_hold = hold
                elif kind == "event":
                    # A row only moves the poll's cursor.
                    self._cursor = max(self._cursor, message["event"]["sequence"])
                if kind not in ("event", "progress"):
                    rows = self._carried_object_lost
            except Exception as error:
                failure = error
                rows = None
                self._carried_object_lost = None
                self._carried_object_last_losses = None
                self._recapture_locked(error)
            current, changed = self._publish_locked()
        if changed:
            self._on_change(current)
        if hold_changed and hold is not None and self._on_program_run_hold is not None:
            self._on_program_run_hold(hold)
        if rows is not None and self._on_carried_object_lost is not None:
            # Also unchanged rows (snapshot, state, heartbeat): a row whose
            # consumer was not ready yet stays eligible on a later message.
            self._on_carried_object_lost(rows)
        if failure is not None:
            raise failure

    def _stream_lost(self, error: BaseException) -> None:
        with self._lock:
            # Unavailable, never stale, until the next snapshot.
            self._drop()
            self._catalog = None
            self._carried_object_lost = None
            self._carried_object_last_losses = None
            current, changed = self._publish_locked()
        if changed:
            self._on_change(current)

    def _recapture_locked(self, error: Exception) -> None:
        _LOGGER.warning("interrupt rule state mirror recaptures: %s", error)
        self._drop()
        self._catalog = None
        self._cursor = 0

    def _apply_stream_state_locked(self, state: Mapping[str, Any]) -> None:
        self._carried_object_lost = _carried_object_lost_rows(
            state.get("carried_object_lost_signals")
        )
        self._carried_object_last_losses = _carried_object_last_losses(
            state.get("carried_object_last_losses")
        )
        if self._carried_object_lost is None or self._carried_object_last_losses is None:
            # One loss state: both known or both unknown.
            self._carried_object_lost = self._carried_object_last_losses = None
        self._apply_rule_states_locked(
            state.get("interrupt_rule_catalog"), state.get("interrupt_rule_state_baseline"),
            new_context_only=False,
        )

    def _apply_rule_states_locked(
        self, catalog_wire: Any, baseline_wire: Any, *, new_context_only: bool
    ) -> None:
        catalog = parse_catalog(catalog_wire)
        self._catalog, self._catalog_read = catalog, True
        baseline = parse_baseline(baseline_wire)
        if (
            not isinstance(baseline, RuleStateBaseline)
            or catalog is None
            or not _proves(catalog, baseline.context)
        ):
            # No ready baseline this catalog proves: unavailable, never clear.
            self._drop()
        elif not new_context_only or baseline.context != self._context:
            self._context = baseline.context
            self._states = dict(baseline.states)

    def carried_object_last_losses(self) -> tuple[Mapping[str, Any], ...] | None:
        """The latest validated ``carried_object_last_losses``
        (coalesced): the last loss per gripper and ``set_key``; None while
        the state is unknown, like the rows."""

        with self._lock:
            return self._carried_object_last_losses

    def carried_object_lost_signals(self) -> tuple[Mapping[str, Any], ...] | None:
        """The latest validated ``carried_object_lost_signals`` rows;
        None while unknown (no snapshot yet or a lost stream)."""

        with self._lock:
            return self._carried_object_lost

    def _poll_locked(self) -> None:
        try:
            response = self._read_events(self._cursor)
            page = _page(response, self._cursor)
            self._apply_rule_states_locked(
                response.get("interrupt_rule_catalog"),
                response.get("interrupt_rule_state_baseline"),
                new_context_only=self._subscribe is not None,
            )
            self._cursor = page["next_after_sequence"]
        except Exception as error:
            # Unreadable reply: recapture.
            self._recapture_locked(error)


__all__ = [
    "InterruptRuleStateMirror",
    "InterruptStateWireError",
    "RULE_STATES",
    "RuleCatalog",
    "RuleStateBaseline",
    "RuleStateContext",
    "parse_baseline",
    "parse_catalog",
]
