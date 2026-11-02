"""Always-current mirror of the Kernel's global interrupt rule states (contract 64).

The existing ``events`` reply carries the outer ``interrupt_rule_state_baseline``
(interrupt-state-events-wire02): null (no installed global rule catalog),
``{"status": "projection_pending"}`` or ``{"status": "ready", "snapshot": ...}``.
The mirror installs a ready baseline whose context the same reply's catalog
proves (SX-2216), then applies only same-context sparse
``interrupt_rule_state_changed`` events whose existing event sequence is
strictly after the baseline's ``through_sequence``. Pending keeps an already
qualified mirror of the same context (it keeps following its sparse events),
except notify rules become unavailable until a ready cut. It otherwise leaves
the states unavailable, retried on the next poll. An
owner change, an unknown catalog or one proving another full context
(SX-2207/2216),
an unreadable page or a malformed row drops the mirror (unavailable, never
clear) and recaptures from the next ready baseline.

The same reply carries ``interrupt_rule_catalog`` (global-rule-runner-shape01,
SX-2184), independent of the baseline's readiness: null (unknown or pending,
never empty), ``{"status": "verified_empty", owner_epoch,
active_publication_generation, bundle, graph}`` (proof of no global rule for
that context) or the installed catalog's context with its sorted Applied
global ``rule_ids`` and the sorted union of their checked condition BB operands
(``referenced_program_blackboard_keys``), optionally with the per-rule
``waiting_for_program`` annotation, or ``{"status": "waiting_for_program",
owner_epoch, active_publication_generation, bundle, graph, rule_ids,
waiting_for_program}`` (decision 110: no Program declares a key of these
rules, so no rule catalog is installed and no state is sent). The mirror keeps
the latest one read; its states are current only while that catalog proves
their full context, and a waiting rule's state is unavailable, never clear.

The same pages carry the existing ``stop_terminal_projected`` rows
(``manyforge.stop_terminal_projection.v1``, SX-2215). The mirror, as the
Runner's one reader of this event stream, keeps the latest bounded set of
them by exact Stop id and generation so an interrupt node can read the
attempted stages of its own Stop; a row is kept as read (a truncated one
proves no attempt) and never affects the rule states.

Delivery is a streaming subscription (``follow``, contract §8.1): the host
streams every new journal row as an ``event`` and the reply's non-page facts
as ``state`` when they change, after a first ``snapshot``. A streamed row is
applied once (by sequence, also against a ``poll`` that read it first); a ready
baseline of a new context installs from a ``snapshot`` or ``state`` with its
``through_sequence`` as the rule-row boundary (the host sends it after every
row it covers, so nothing is reread). A dead link makes the states
unavailable until the next snapshot. ``poll`` stays the one-shot read for a
caller that needs a fresh catalog or Stop terminal at once.
"""

from __future__ import annotations

from collections import OrderedDict
from collections.abc import Callable, Mapping, Sequence
import logging
from threading import Event, Lock
from typing import Any, Final

from manyforge_headless_api.execution_event_stream import follow_execution_events

# The wire parsers are shared with the Headless GUI projection.
from manyforge_headless_api.global_rule_status import (
    RULE_STATES,
    STATE_EVENT_KIND,
    InterruptStateWireError,
    RuleCatalog,
    RuleStateBaseline,
    RuleStateContext,
    catalog_proves as _proves,
    parse_baseline,
    parse_catalog,
    parse_state_event,
)

# One poll reads at most this many 16-row pages before yielding.
MAXIMUM_PAGES_PER_POLL: Final = 8
STOP_TERMINAL_EVENT_KIND: Final = "stop_terminal_projected"
STOP_TERMINAL_API_VERSION: Final = "manyforge.stop_terminal_projection.v1"
# Stop terminals retained for interrupt nodes (oldest dropped first).
MAXIMUM_STOP_TERMINALS: Final = 32
_MAX_SAFE: Final = 9_007_199_254_740_991

_LOGGER = logging.getLogger(__name__)


def _page(response: Mapping[str, Any], owner_epoch: str, after: int) -> Mapping[str, Any]:
    page = response.get("event_page")
    if (
        response.get("command") != "events"
        or response.get("owner_epoch") != owner_epoch
        or not isinstance(page, Mapping)
        or page.get("owner_epoch") != owner_epoch
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
    """The §8.1/§10 rows, strictly: closed keys, non-empty strings (``set_key``
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
    """The §10 last losses, strictly: closed keys, all non-empty strings, one
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
    """Follow the Kernel's global rule states through the events reply.

    ``read_events(owner_epoch, after_sequence)`` is the existing events
    command; ``on_change(states)`` receives ``None`` (unavailable) or the
    complete current rule_id → state mapping whenever that changes.
    """

    def __init__(
        self,
        *,
        owner_epoch: Callable[[], str],
        read_events: Callable[[str, int], Mapping[str, Any]],
        on_change: Callable[[Mapping[str, str] | None], None],
        subscribe: Callable[[int, str | None], Any] | None = None,
        on_carried_object_lost: Callable[[tuple[Mapping[str, Any], ...]], None] | None = None,
    ) -> None:
        self._owner_epoch = owner_epoch
        self._read_events = read_events
        self._on_change = on_change
        # The event subscription (``follow``) and the latest
        # ``carried_object_lost_signals`` rows read, given to their consumer.
        self._subscribe = subscribe
        self._on_carried_object_lost = on_carried_object_lost
        # None: unknown (no snapshot yet, a gap, a lost stream): nothing is
        # offered then, never an inferred empty set (SX-194).
        self._carried_object_lost: tuple[Mapping[str, Any], ...] | None = None
        # Contract §10 "Coalescing": the last loss per gripper and key; known
        # and unknown together with the rows.
        self._carried_object_last_losses: tuple[Mapping[str, Any], ...] | None = None
        # Streamed rule rows at or below it are covered by the installed
        # baseline.
        self._state_boundary = 0
        self._notify_boundary = 0
        self._notify_sequences: dict[str, int] = {}
        self._lock = Lock()
        self._owner: str | None = None
        self._context: RuleStateContext | None = None
        self._cursor = 0
        self._states: dict[str, str] = {}
        self._notify_pending = False
        self._published: Mapping[str, str] | None | bool = False
        # The latest installed global catalog read (None: none installed or
        # unreadable); `catalog_read` is False until a reply was read.
        self._catalog: RuleCatalog | None = None
        self._catalog_read = False
        self._stop_terminals: OrderedDict[tuple[str, int], Mapping[str, Any]] = OrderedDict()
        # The last page read said more rows follow (False after an error).
        self._has_more = False

    def current(
        self, context: tuple[str, int, str, str] | None = None
    ) -> Mapping[str, str] | None:
        """The current states, or None (unavailable). Given a full context
        (owner epoch, publication, bundle, graph), None unless the mirror's
        states are of exactly that context."""

        with self._lock:
            states = self._current_locked()
            if states is None or context is None:
                return states
            own = self._context
            assert own is not None
            if context != (
                own.owner_epoch, own.active_publication_generation,
                own.retained_bundle_id, own.graph_revision,
            ):
                return None
            return states

    def _current_locked(self) -> dict[str, str] | None:
        # Current only while the latest catalog proves exactly their context
        # (SX-2216): an unknown (None) catalog proves none. A rule waiting for
        # a Program (decision 110) is unavailable, never clear.
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

    def stop_terminal(
        self, stop_id: str, stop_generation: int, *, read_to_end: bool = False
    ) -> Mapping[str, Any] | None:
        """The retained Stop terminal projection of exactly this Stop; None
        when not (yet) read. ``read_to_end`` (only after read_stop reported
        ``recorded``, SX-2226): first read the events, after any read already
        in flight, until the exact record or ``has_more`` is false; a reply
        error ends the read (unproven)."""

        key = (stop_id, stop_generation)
        with self._lock:
            terminal = self._stop_terminals.get(key)
        while terminal is None and read_to_end:
            self.poll(wait=True)
            with self._lock:
                terminal = self._stop_terminals.get(key)
                read_to_end = self._has_more
        return terminal

    def _retain_stop_terminal(self, owner: str, event: Mapping[str, Any]) -> None:
        payload = event.get("payload")
        if (
            not isinstance(payload, Mapping)
            or payload.get("api_version") != STOP_TERMINAL_API_VERSION
            or payload.get("owner_epoch") != owner
            or type(payload.get("stop_id")) is not str
            or not payload["stop_id"]
            or type(payload.get("stop_generation")) is not int
        ):
            _LOGGER.warning("Stop terminal row %s is not usable", event.get("sequence"))
            return
        key = (payload["stop_id"], payload["stop_generation"])
        self._stop_terminals.pop(key, None)
        self._stop_terminals[key] = payload
        while len(self._stop_terminals) > MAXIMUM_STOP_TERMINALS:
            self._stop_terminals.popitem(last=False)

    def _drop(self) -> None:
        self._context = None
        self._states = {}
        self._notify_boundary = 0
        self._notify_sequences = {}
        self._notify_pending = False

    def poll(self, *, wait: bool = False) -> None:
        """One bounded read on the existing cadence (never concurrent); with
        ``wait``, after a read already in flight rather than skipping."""

        if not self._lock.acquire(blocking=wait):
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

    # Event subscription (contract §8.1) -------------------------------------

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
        with self._lock:
            try:
                owner = message["owner_epoch"]
                if kind == "gap":
                    self._carried_object_lost = None
                    self._carried_object_last_losses = None  # Until its snapshot.
                if kind == "snapshot" and owner != self._owner:
                    self._owner, self._cursor, self._state_boundary = owner, 0, 0
                    self._drop()
                    self._stop_terminals.clear()
                elif kind != "gap" and owner != self._owner:
                    # A gap names the new owner; its snapshot follows.
                    raise InterruptStateWireError("stream message names another Kernel owner")
                if kind in ("snapshot", "state"):
                    self._apply_stream_state_locked(owner, message["state"])
                elif kind == "event":
                    self._apply_stream_event_locked(owner, message["event"])
                if kind not in ("event", "progress", "gap"):
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
        self._has_more = False
        _LOGGER.warning("interrupt rule state mirror recaptures: %s", error)
        self._drop()
        self._catalog = None
        self._cursor = 0
        self._state_boundary = 0

    def _apply_stream_state_locked(self, owner: str, state: Mapping[str, Any]) -> None:
        self._carried_object_lost = _carried_object_lost_rows(
            state.get("carried_object_lost_signals")
        )
        self._carried_object_last_losses = _carried_object_last_losses(
            state.get("carried_object_last_losses")
        )
        if self._carried_object_lost is None or self._carried_object_last_losses is None:
            # One loss state: both known or both unknown.
            self._carried_object_lost = self._carried_object_last_losses = None
        catalog = parse_catalog(state.get("interrupt_rule_catalog"))
        if catalog is not None and catalog.owner_epoch != owner:
            raise InterruptStateWireError("rule catalog names another Kernel owner")
        self._catalog, self._catalog_read = catalog, True
        if self._context is not None and (catalog is None or not _proves(catalog, self._context)):
            self._drop()
        baseline = parse_baseline(state.get("interrupt_rule_state_baseline"))
        if baseline is None:
            self._drop()
        elif baseline == "projection_pending":
            self._invalidate_pending_notify_locked(catalog)
        elif isinstance(baseline, RuleStateBaseline):
            if baseline.context.owner_epoch != owner:
                raise InterruptStateWireError("baseline names another Kernel owner")
            if catalog is None or not _proves(catalog, baseline.context):
                self._drop()
            elif baseline.context != self._context:
                # Every rule row at or below the cut was sent before it.
                self._context = baseline.context
                self._states = dict(baseline.states)
                self._state_boundary = baseline.through_sequence
                self._notify_boundary = baseline.through_sequence
                self._notify_pending = False
            elif baseline.through_sequence >= self._notify_boundary:
                self._restore_notify_locked(catalog, baseline)

    def _notify_needs_cut_locked(self, catalog: RuleCatalog, baseline: RuleStateBaseline) -> bool:
        return self._notify_pending or any(
            self._notify_sequences.get(rule_id, 0) <= baseline.through_sequence
            and self._states.get(rule_id) != baseline.states.get(rule_id)
            for rule_id in catalog.notify_rule_ids
        )

    def _invalidate_pending_notify_locked(self, catalog: RuleCatalog | None) -> None:
        if catalog is not None and catalog.notify_rule_ids:
            for rule_id in catalog.notify_rule_ids:
                self._states.pop(rule_id, None)
                self._notify_sequences.pop(rule_id, None)
            self._notify_pending = True

    def _restore_notify_locked(self, catalog: RuleCatalog, baseline: RuleStateBaseline) -> None:
        for rule_id in catalog.notify_rule_ids:
            if self._notify_sequences.get(rule_id, 0) > baseline.through_sequence:
                continue
            if rule_id in baseline.states:
                self._states[rule_id] = baseline.states[rule_id]
            else:
                self._states.pop(rule_id, None)
            self._notify_sequences.pop(rule_id, None)
        self._notify_boundary = baseline.through_sequence
        self._notify_pending = False

    def _apply_stream_event_locked(self, owner: str, event: Mapping[str, Any]) -> None:
        sequence = event.get("sequence")
        if type(sequence) is not int or sequence <= self._cursor:
            return  # Already applied (a poll read it first).
        self._cursor = sequence
        if event.get("record_kind") == STOP_TERMINAL_EVENT_KIND:
            self._retain_stop_terminal(owner, event)
            return
        if (
            event.get("record_kind") != STATE_EVENT_KIND
            or sequence <= self._state_boundary
            or self._context is None
        ):
            return
        context, rule_id, state = parse_state_event(event.get("payload"))
        if context != self._context:
            self._drop()  # Replacement before its baseline arrived.
            return
        if (self._catalog is not None and rule_id in self._catalog.notify_rule_ids
                and (self._notify_pending or sequence <= self._notify_boundary)):
            return
        self._states[rule_id] = state
        if self._catalog is not None and rule_id in self._catalog.notify_rule_ids:
            self._notify_sequences[rule_id] = sequence

    def carried_object_last_losses(self) -> tuple[Mapping[str, Any], ...] | None:
        """The latest validated ``carried_object_last_losses`` (contract §10
        "Coalescing"): the last loss per gripper and ``set_key``; None while
        the state is unknown, like the rows."""

        with self._lock:
            return self._carried_object_last_losses

    def carried_object_lost_signals(self) -> tuple[Mapping[str, Any], ...] | None:
        """The latest validated ``carried_object_lost_signals`` rows (contract
        §8.1); None while unknown (no snapshot yet, a gap or a lost stream)."""

        with self._lock:
            return self._carried_object_lost

    def _poll_locked(self) -> None:
        try:
            owner = self._owner_epoch()
            if owner != self._owner:
                self._owner, self._cursor = owner, 0
                self._drop()
                self._stop_terminals.clear()
            for _page_index in range(MAXIMUM_PAGES_PER_POLL):
                if not self._read_one_page(owner):
                    return
        except Exception as error:
            # Gap, unreadable reply or malformed row: recapture.
            self._recapture_locked(error)

    def _read_one_page(self, owner: str) -> bool:
        """Read one page; True when another page should follow at once."""

        response = self._read_events(owner, self._cursor)
        page = _page(response, owner, self._cursor)
        catalog = parse_catalog(response.get("interrupt_rule_catalog"))
        if catalog is not None and catalog.owner_epoch != owner:
            raise InterruptStateWireError("rule catalog names another Kernel owner")
        self._catalog, self._catalog_read = catalog, True
        if self._context is not None and (catalog is None or not _proves(catalog, self._context)):
            # The catalog is unknown or proves another context (SX-2216): the
            # prior states are no longer current (unavailable) until a ready
            # cut of the context a catalog proves.
            self._drop()
        baseline = parse_baseline(response.get("interrupt_rule_state_baseline"))
        boundary = self._cursor
        if baseline is None:
            # No installed global rule catalog: unknown, never clear.
            self._drop()
        elif baseline == "projection_pending":
            self._invalidate_pending_notify_locked(catalog)
        elif isinstance(baseline, RuleStateBaseline):
            if baseline.context.owner_epoch != owner:
                raise InterruptStateWireError("baseline names another Kernel owner")
            if catalog is None or not _proves(catalog, baseline.context):
                # A cut without a catalog proving its context: wait for one
                # reply where both agree (unavailable, never inferred).
                self._drop()
            elif baseline.context != self._context:
                if self._cursor > baseline.through_sequence:
                    # Rows after the cut were already passed: reread them.
                    self._cursor = baseline.through_sequence
                    self._drop()
                    return True
                self._context = baseline.context
                self._states = dict(baseline.states)
                self._notify_boundary = baseline.through_sequence
                self._notify_pending = False
                boundary = baseline.through_sequence
            elif baseline.through_sequence >= self._notify_boundary:
                needs_cut = self._notify_needs_cut_locked(catalog, baseline)
                self._restore_notify_locked(catalog, baseline)
                if needs_cut and self._cursor > baseline.through_sequence:
                    self._cursor = baseline.through_sequence
                    return True
        for event in page["events"]:
            if event.get("record_kind") == STOP_TERMINAL_EVENT_KIND:
                self._retain_stop_terminal(owner, event)
                continue
            if event["sequence"] <= boundary or event.get("record_kind") != STATE_EVENT_KIND:
                continue
            if self._context is None:
                # Pending without a qualified mirror: wait for the baseline.
                continue
            context, rule_id, state = parse_state_event(event.get("payload"))
            if context != self._context:
                # Replacement before its baseline arrived: recapture.
                self._drop()
                continue
            if (catalog is not None and rule_id in catalog.notify_rule_ids
                    and (self._notify_pending or event["sequence"] <= self._notify_boundary)):
                continue
            self._states[rule_id] = state
            if catalog is not None and rule_id in catalog.notify_rule_ids:
                self._notify_sequences[rule_id] = event["sequence"]
        self._cursor = max(page["next_after_sequence"], boundary)
        self._has_more = page["has_more"]
        return page["has_more"]


__all__ = [
    "InterruptRuleStateMirror",
    "InterruptStateWireError",
    "MAXIMUM_PAGES_PER_POLL",
    "RULE_STATES",
    "RuleCatalog",
    "RuleStateBaseline",
    "RuleStateContext",
    "STATE_EVENT_KIND",
    "STOP_TERMINAL_EVENT_KIND",
    "parse_baseline",
    "parse_catalog",
    "parse_state_event",
]
