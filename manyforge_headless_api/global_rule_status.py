"""Global interrupt rule wire of the existing ``events`` reply (contract 64).

The one parser of ``interrupt_rule_catalog``, ``interrupt_rule_state_baseline``
and the sparse ``interrupt_rule_state_changed`` rows (see the Runner's
``interrupt_state_mirror`` for their meaning), shared by the Runner mirror and
the Headless GUI projection, plus that projection: one row per Applied global
rule with its live state for the operator.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass
from typing import Any, Final


STATE_EVENT_KIND: Final = "interrupt_rule_state_changed"
RULE_STATES: Final = frozenset({"clear", "tripped", "latched", "unknown"})

_MAX_SAFE: Final = 9_007_199_254_740_991
_MAX_UINT64: Final = 2**64 - 1
_CONTEXT_FIELDS: Final = (
    "owner_epoch",
    "catalog_generation",
    "active_publication_generation",
    "retained_bundle_id",
    "graph_revision",
)


class InterruptStateWireError(ValueError):
    pass


@dataclass(frozen=True, slots=True)
class RuleStateContext:
    owner_epoch: str
    catalog_generation: str
    active_publication_generation: int
    retained_bundle_id: str
    graph_revision: str


@dataclass(frozen=True, slots=True)
class RuleStateBaseline:
    context: RuleStateContext
    through_sequence: int
    states: Mapping[str, str]
    unavailable_reasons: Mapping[str, Mapping[str, Any]]


def _closed(value: Any, fields: set[str]) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or set(value) != fields:
        raise InterruptStateWireError("interrupt rule state object is not closed")
    return value


def _text(value: Any) -> str:
    if type(value) is not str or not value:
        raise InterruptStateWireError("identity must be nonempty text")
    return value


def _uint64(value: Any, *, minimum: int = 0) -> str:
    if (
        type(value) is not str
        or not value.isascii()
        or not value.isdigit()
        or value != str(int(value))
        or not minimum <= int(value) <= _MAX_UINT64
    ):
        raise InterruptStateWireError("counter must be a canonical uint64 decimal")
    return value


def _acquisition(value: Any) -> None:
    if value is None:
        return
    row = _closed(value, {"source_id", "source_epoch", "source_sequence", "source_cursor"})
    _text(row["source_id"])
    _text(row["source_epoch"])
    _uint64(row["source_sequence"])
    cursor = _closed(
        row["source_cursor"], {"owner_epoch", "catalog_generation", "source_id", "record_sequence"}
    )
    _text(cursor["owner_epoch"])
    _uint64(cursor["catalog_generation"], minimum=1)
    _text(cursor["source_id"])
    _uint64(cursor["record_sequence"])


def _state(value: Any) -> str:
    if value not in RULE_STATES:
        raise InterruptStateWireError("unknown interrupt rule state")
    return value


def _context(value: Mapping[str, Any]) -> RuleStateContext:
    generation = value["active_publication_generation"]
    if type(generation) is not int or not 1 <= generation <= _MAX_SAFE:
        raise InterruptStateWireError("publication generation must be a positive safe integer")
    return RuleStateContext(
        owner_epoch=_text(value["owner_epoch"]),
        catalog_generation=_uint64(value["catalog_generation"], minimum=1),
        active_publication_generation=generation,
        retained_bundle_id=_text(value["retained_bundle_id"]),
        graph_revision=_text(value["graph_revision"]),
    )


_CATALOG_FIELDS: Final = frozenset(
    {
        "owner_epoch",
        "catalog_generation",
        "active_publication_generation",
        "bundle",
        "graph",
        "rule_ids",
        "referenced_program_blackboard_keys",
    }
)


_EMPTY_CATALOG_FIELDS: Final = frozenset(
    {"status", "owner_epoch", "active_publication_generation", "bundle", "graph"}
)
_WAITING_CATALOG_FIELDS: Final = _EMPTY_CATALOG_FIELDS | {"rule_ids", "waiting_for_program"}


@dataclass(frozen=True, slots=True)
class RuleCatalog:
    owner_epoch: str
    # None for a verified-empty catalog (no installed rule catalog).
    catalog_generation: str | None
    active_publication_generation: int
    bundle: str
    graph: str
    rule_ids: tuple[str, ...]
    referenced_program_blackboard_keys: tuple[str, ...]
    # Rules waiting for a Program that declares their keys (decision 110):
    # (rule_id, keys) rows sorted by rule id; all rules of a waiting catalog.
    waiting_for_program: tuple[tuple[str, tuple[str, ...]], ...] = ()
    notify_rule_ids: tuple[str, ...] = ()

    @property
    def waiting_rule_ids(self) -> frozenset[str]:
        return frozenset(rule_id for rule_id, _keys in self.waiting_for_program)

    @property
    def context(self) -> tuple[str, int, str, str]:
        """The full context a source compares (SX-2184)."""

        return (self.owner_epoch, self.active_publication_generation, self.bundle, self.graph)


def catalog_proves(catalog: RuleCatalog, context: RuleStateContext) -> bool:
    """The catalog names exactly this state context (SX-2207)."""

    return (
        catalog.owner_epoch == context.owner_epoch
        and catalog.catalog_generation == context.catalog_generation
        and catalog.active_publication_generation == context.active_publication_generation
        and catalog.bundle == context.retained_bundle_id
        and catalog.graph == context.graph_revision
    )


def _sorted_names(value: Any) -> tuple[str, ...]:
    if not isinstance(value, list):
        raise InterruptStateWireError("catalog names must be a list")
    names = tuple(_text(item) for item in value)
    if list(names) != sorted(set(names)):
        raise InterruptStateWireError("catalog names are not sorted and unique")
    return names


def _nonempty_sorted_names(value: Any) -> tuple[str, ...]:
    names = _sorted_names(value)
    if not names:
        raise InterruptStateWireError("catalog names must be a nonempty list")
    return names


def _waiting_rows(
    value: Any, rule_ids: tuple[str, ...]
) -> tuple[tuple[str, tuple[str, ...]], ...]:
    """The closed per-rule rows, sorted by unique rule id among the catalog's
    rules, each with its nonempty sorted unique keys (canonical like every
    other list of this wire)."""

    if not isinstance(value, list):
        raise InterruptStateWireError("waiting_for_program must be a list")
    rows: list[tuple[str, tuple[str, ...]]] = []
    for item in value:
        row = _closed(item, {"rule_id", "keys"})
        rule_id = _text(row["rule_id"])
        if rule_id not in rule_ids or (rows and rule_id <= rows[-1][0]):
            raise InterruptStateWireError("waiting rule is unknown, repeated or not sorted")
        rows.append((rule_id, _nonempty_sorted_names(row["keys"])))
    return tuple(rows)


def _notify_ids(row: Mapping[str, Any], rule_ids: tuple[str, ...]) -> tuple[str, ...]:
    notify = _sorted_names(row.get("notify_rule_ids", []))
    if not set(notify) <= set(rule_ids):
        raise InterruptStateWireError("notify rule is not in the catalog")
    return notify


def _state_row(value: Any, fields: set[str]) -> Mapping[str, Any]:
    unknown = isinstance(value, Mapping) and value.get("state") == "unknown"
    row = _closed(value, fields | ({"unavailable_reason"} if unknown else set()))
    _state(row["state"])
    _acquisition(row["triggering_acquisition"])
    if unknown:
        if row["triggering_acquisition"] is not None:
            raise InterruptStateWireError("unknown state has an acquisition")
        reason = _closed(row["unavailable_reason"], {"code", "message", "details"})
        _text(reason["code"])
        if type(reason["message"]) is not str or not isinstance(reason["details"], Mapping):
            raise InterruptStateWireError("invalid unavailable reason")
        if any(type(k) is not str or type(v) is not str for k, v in reason["details"].items()):
            raise InterruptStateWireError("invalid unavailable reason details")
    return row


def _unavailable_reason(row: Mapping[str, Any]) -> Mapping[str, Any] | None:
    if row["state"] != "unknown":
        return None
    reason = row["unavailable_reason"]
    return {
        "code": reason["code"],
        "message": reason["message"],
        "details": dict(reason["details"]),
    }


def parse_catalog(value: Any) -> RuleCatalog | None:
    """None (unknown or pending), a verified-empty, a waiting-for-Program or
    the installed catalog."""

    if value is None:
        return None
    notify_field = {"notify_rule_ids"} if isinstance(value, Mapping) and "notify_rule_ids" in value else set()
    if isinstance(value, Mapping) and value.get("status") == "waiting_for_program":
        row = _closed(value, set(_WAITING_CATALOG_FIELDS) | notify_field)
    elif isinstance(value, Mapping) and "status" in value:
        row = _closed(value, set(_EMPTY_CATALOG_FIELDS))
        if row["status"] != "verified_empty":
            raise InterruptStateWireError("unknown interrupt rule catalog status")
    elif isinstance(value, Mapping) and "waiting_for_program" in value:
        row = _closed(value, set(_CATALOG_FIELDS | {"waiting_for_program"}) | notify_field)
    else:
        row = _closed(value, set(_CATALOG_FIELDS) | notify_field)
    generation = row["active_publication_generation"]
    if type(generation) is not int or not 1 <= generation <= _MAX_SAFE:
        raise InterruptStateWireError("publication generation must be a positive safe integer")
    if row.get("status") == "waiting_for_program":
        # Every rule of this product waits (decision 110): exact coverage.
        rule_ids = _nonempty_sorted_names(row["rule_ids"])
        waiting = _waiting_rows(row["waiting_for_program"], rule_ids)
        notify = _notify_ids(row, rule_ids)
        if tuple(rule_id for rule_id, _keys in waiting) != rule_ids:
            raise InterruptStateWireError("a waiting catalog must name every rule as waiting")
        return RuleCatalog(
            owner_epoch=_text(row["owner_epoch"]),
            catalog_generation=None,
            active_publication_generation=generation,
            bundle=_text(row["bundle"]),
            graph=_text(row["graph"]),
            rule_ids=rule_ids,
            referenced_program_blackboard_keys=(),
            waiting_for_program=waiting,
            notify_rule_ids=notify,
        )
    if "status" in row:
        return RuleCatalog(
            owner_epoch=_text(row["owner_epoch"]),
            catalog_generation=None,
            active_publication_generation=generation,
            bundle=_text(row["bundle"]),
            graph=_text(row["graph"]),
            rule_ids=(),
            referenced_program_blackboard_keys=(),
        )
    rule_ids = _sorted_names(row["rule_ids"])
    return RuleCatalog(
        owner_epoch=_text(row["owner_epoch"]),
        catalog_generation=_uint64(row["catalog_generation"], minimum=1),
        active_publication_generation=generation,
        bundle=_text(row["bundle"]),
        graph=_text(row["graph"]),
        rule_ids=rule_ids,
        referenced_program_blackboard_keys=_sorted_names(row["referenced_program_blackboard_keys"]),
        waiting_for_program=_waiting_rows(row.get("waiting_for_program", []), rule_ids),
        notify_rule_ids=_notify_ids(row, rule_ids),
    )


def parse_baseline(value: Any) -> RuleStateBaseline | str | None:
    """None (no catalog), "projection_pending" or the ready baseline."""

    if value is None:
        return None
    if isinstance(value, Mapping) and value.get("status") == "projection_pending":
        _closed(value, {"status"})
        return "projection_pending"
    ready = _closed(value, {"status", "snapshot"})
    if ready["status"] != "ready":
        raise InterruptStateWireError("unknown interrupt rule baseline status")
    snapshot = _closed(ready["snapshot"], {*_CONTEXT_FIELDS, "through_sequence", "states"})
    through = snapshot["through_sequence"]
    if type(through) is not int or not 0 <= through <= _MAX_SAFE:
        raise InterruptStateWireError("through_sequence must be a nonnegative safe integer")
    rows = snapshot["states"]
    if not isinstance(rows, list):
        raise InterruptStateWireError("baseline states must be a list")
    states: dict[str, str] = {}
    unavailable_reasons: dict[str, Mapping[str, Any]] = {}
    for row in rows:
        row = _state_row(row, {"rule_id", "state", "triggering_acquisition"})
        rule_id = _text(row["rule_id"])
        if states and rule_id <= max(states):
            raise InterruptStateWireError("baseline states are not sorted and unique")
        states[rule_id] = _state(row["state"])
        reason = _unavailable_reason(row)
        if reason is not None:
            unavailable_reasons[rule_id] = reason
    return RuleStateBaseline(_context(snapshot), through, states, unavailable_reasons)


def parse_state_event(payload: Any) -> tuple[RuleStateContext, str, str]:
    row = _state_row(payload, {*_CONTEXT_FIELDS, "rule_id", "state", "triggering_acquisition"})
    return _context(row), _text(row["rule_id"]), _state(row["state"])



# The GUI state of a rule with no known current state (no qualified baseline
# of the catalog's context yet). Generic: covers a first reading not yet taken and
# a pending projection update alike; the wire has no per-rule source-waiting reason.
PENDING_STATE: Final = "pending"
WAITING_FOR_PROGRAM_STATE: Final = "waiting_for_program"


class GlobalRuleStatusModel:
    """The operator view of the Kernel's global rule states (Headless GUI).

    Folds each ``events`` reply the Headless event pump already reads, with
    the Runner mirror's rules: states are current only while the reply's
    catalog proves the ready baseline's full context; a pending baseline
    keeps the last qualified one except notify rules, which become unavailable;
    null drops it. A ready cut supersedes rows
    at or before its ``through_sequence``; later same-context sparse rows
    overlay it (only the latest row per rule is kept). ``rows()`` is None
    while the catalog is unknown (never empty), otherwise one row per catalog
    rule: clear/tripped/latched, ``waiting_for_program`` with the keys no
    Program declares (decision 110), or ``pending`` while no state is known.
    """

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        """Forget everything (a new Kernel owner)."""

        self._latest: dict[
            str, tuple[RuleStateContext, int, str, Mapping[str, Any] | None]
        ] = {}
        # No baseline cut before this sequence is current (see observe).
        self._floor = 0
        self.unknown()

    def unknown(self) -> None:
        """Unknown until the next reply (unreadable page); the rows already
        read stay, since the reader resumes after them."""

        self._catalog: RuleCatalog | None = None
        self._baseline: RuleStateBaseline | None = None
        self._pending_notify: set[str] = set()

    def observe(
        self,
        owner_epoch: str,
        catalog: Any,
        baseline: Any,
        events: Iterable[Mapping[str, Any]],
    ) -> None:
        """Fold one reply. A wire error makes the states unknown and is
        raised; its page's rows are lost, so only a later cut covering them
        is current again."""

        events = list(events)
        try:
            parsed_catalog = parse_catalog(catalog)
            if parsed_catalog is not None and parsed_catalog.owner_epoch != owner_epoch:
                raise InterruptStateWireError("rule catalog names another Kernel owner")
            parsed_baseline = parse_baseline(baseline)
            if isinstance(parsed_baseline, RuleStateBaseline):
                if parsed_baseline.context.owner_epoch != owner_epoch:
                    raise InterruptStateWireError("baseline names another Kernel owner")
                if (
                    parsed_catalog is None
                    or not catalog_proves(parsed_catalog, parsed_baseline.context)
                    or parsed_baseline.through_sequence < self._floor
                ):
                    parsed_baseline = None
            elif parsed_baseline == "projection_pending":
                # Keep the qualified cut only while this catalog still proves it.
                prior = self._baseline
                parsed_baseline = (
                    prior if prior is not None and parsed_catalog is not None
                    and catalog_proves(parsed_catalog, prior.context) else None
                )
            latest = dict(self._latest)
            pending_notify = (
                set(parsed_catalog.notify_rule_ids)
                if parsed_catalog is not None and baseline is not None
                and baseline.get("status") == "projection_pending" else set()
            )
            for rule_id in pending_notify:
                latest.pop(rule_id, None)
            for event in events:
                if event.get("record_kind") == STATE_EVENT_KIND:
                    payload = event.get("payload")
                    context, rule_id, state = parse_state_event(payload)
                    if rule_id not in pending_notify:
                        reason = _unavailable_reason(payload)
                        latest[rule_id] = (context, event["sequence"], state, reason)
        except Exception:
            self.unknown()
            self._floor = max(
                [self._floor, *(
                    event["sequence"] for event in events
                    if isinstance(event, Mapping) and type(event.get("sequence")) is int
                )]
            )
            raise
        self._catalog = parsed_catalog
        self._baseline = parsed_baseline
        self._pending_notify = pending_notify
        if parsed_catalog is not None:
            # Bounded by the current catalog's rules.
            latest = {
                rule_id: row for rule_id, row in latest.items()
                if rule_id in parsed_catalog.rule_ids
            }
        self._latest = latest

    def rows(self) -> list[dict[str, Any]] | None:
        catalog = self._catalog
        if catalog is None:
            return None
        baseline = self._baseline
        states: dict[str, str] = {}
        unavailable_reasons: dict[str, Mapping[str, Any]] = {}
        if baseline is not None:
            states = dict(baseline.states)
            unavailable_reasons = dict(baseline.unavailable_reasons)
            for rule_id, (context, sequence, state, reason) in self._latest.items():
                if context == baseline.context and sequence > baseline.through_sequence:
                    states[rule_id] = state
                    if reason is None:
                        unavailable_reasons.pop(rule_id, None)
                    else:
                        unavailable_reasons[rule_id] = reason
        waiting = dict(catalog.waiting_for_program)
        for rule_id in self._pending_notify:
            states.pop(rule_id, None)
            unavailable_reasons.pop(rule_id, None)
        rows: list[dict[str, Any]] = []
        for rule_id in catalog.rule_ids:
            state = (
                WAITING_FOR_PROGRAM_STATE if rule_id in waiting
                else states.get(rule_id, PENDING_STATE)
            )
            row: dict[str, Any] = {
                "rule_id": rule_id,
                "state": state,
                "keys": list(waiting.get(rule_id, ())),
            }
            if state == "unknown":
                reason = unavailable_reasons.get(rule_id)
                if reason is None:
                    raise InterruptStateWireError("unknown rule state has no unavailable reason")
                row["unavailable_reason"] = dict(reason)
            rows.append(row)
        return rows


__all__ = [
    "GlobalRuleStatusModel",
    "InterruptStateWireError",
    "PENDING_STATE",
    "RULE_STATES",
    "RuleCatalog",
    "RuleStateBaseline",
    "RuleStateContext",
    "STATE_EVENT_KIND",
    "WAITING_FOR_PROGRAM_STATE",
    "catalog_proves",
    "parse_baseline",
    "parse_catalog",
    "parse_state_event",
]
