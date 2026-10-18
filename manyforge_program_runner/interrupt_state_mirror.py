"""Always-current mirror of the Kernel's global interrupt rule states (contract 64).

The existing ``events`` reply carries the outer ``interrupt_rule_state_baseline``
(interrupt-state-events-wire02): null (no installed global rule catalog),
``{"status": "projection_pending"}`` or ``{"status": "ready", "snapshot": ...}``.
The mirror installs a ready baseline whose context the same reply's catalog
proves (SX-2216), then applies only same-context sparse
``interrupt_rule_state_changed`` events whose existing event sequence is
strictly after the baseline's ``through_sequence``. Pending keeps an already
qualified mirror of the same context (it keeps following its sparse events)
and otherwise leaves the states unavailable, retried on the next poll. An
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
"""

from __future__ import annotations

from collections import OrderedDict
from collections.abc import Callable, Mapping
import logging
from threading import Lock
from typing import Any, Final

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
    ) -> None:
        self._owner_epoch = owner_epoch
        self._read_events = read_events
        self._on_change = on_change
        self._lock = Lock()
        self._owner: str | None = None
        self._context: RuleStateContext | None = None
        self._cursor = 0
        self._states: dict[str, str] = {}
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
        return {rule_id: state for rule_id, state in self._states.items() if rule_id not in waiting}

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

    def poll(self, *, wait: bool = False) -> None:
        """One bounded read on the existing cadence (never concurrent); with
        ``wait``, after a read already in flight rather than skipping."""

        if not self._lock.acquire(blocking=wait):
            return
        try:
            self._poll_locked()
            current = self._current_locked()
            changed = current != self._published
            self._published = current
        finally:
            self._lock.release()
        if changed:
            self._on_change(current)

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
            self._has_more = False
            # Gap, unreadable reply or malformed row: recapture.
            _LOGGER.warning("interrupt rule state mirror recaptures: %s", error)
            self._drop()
            self._catalog = None
            self._cursor = 0

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
                boundary = baseline.through_sequence
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
            self._states[rule_id] = state
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
