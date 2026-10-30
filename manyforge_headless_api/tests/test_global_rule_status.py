"""Headless GUI projection of the Kernel's global rule states (contract 64)."""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_headless_api.global_rule_status import (
    GlobalRuleStatusModel,
    InterruptStateWireError,
)

_OWNER = "kernel-1"
_CONTEXT = {
    "owner_epoch": _OWNER,
    "catalog_generation": "4",
    "active_publication_generation": 3,
    "retained_bundle_id": "bundle-1",
    "graph_revision": "graph-1",
}


def _catalog(rule_ids: list[str], **extra: Any) -> dict[str, Any]:
    return {
        "owner_epoch": _OWNER,
        "catalog_generation": _CONTEXT["catalog_generation"],
        "active_publication_generation": 3,
        "bundle": "bundle-1",
        "graph": "graph-1",
        "rule_ids": rule_ids,
        "referenced_program_blackboard_keys": [],
        **extra,
    }


def _ready(
    states: dict[str, str], through: int,
    unavailable_reasons: dict[str, dict[str, Any]] | None = None,
    **context: Any,
) -> dict[str, Any]:
    return {
        "status": "ready",
        "snapshot": {
            **_CONTEXT,
            **context,
            "through_sequence": through,
            "states": [
                {
                    "rule_id": rule_id,
                    "state": state,
                    "triggering_acquisition": None,
                    **({"unavailable_reason": unavailable_reasons[rule_id]}
                       if unavailable_reasons is not None and rule_id in unavailable_reasons else {}),
                }
                for rule_id, state in sorted(states.items())
            ],
        },
    }


def _row(
    sequence: int, rule_id: str, state: str,
    unavailable_reason: dict[str, Any] | None = None,
    **context: Any,
) -> dict[str, Any]:
    return {
        "sequence": sequence,
        "record_kind": "interrupt_rule_state_changed",
        "payload": {
            **_CONTEXT,
            **context,
            "rule_id": rule_id,
            "state": state,
            "triggering_acquisition": None,
            **({"unavailable_reason": unavailable_reason} if unavailable_reason is not None else {}),
        },
    }


def _states(model: GlobalRuleStatusModel) -> dict[str, str] | None:
    rows = model.rows()
    return None if rows is None else {row["rule_id"]: row["state"] for row in rows}


def test_unknown_catalog_is_unknown_and_verified_empty_is_no_rules() -> None:
    model = GlobalRuleStatusModel()
    assert model.rows() is None
    model.observe(_OWNER, None, None, [])
    assert model.rows() is None
    model.observe(
        _OWNER,
        {"status": "verified_empty", "owner_epoch": _OWNER,
         "active_publication_generation": 3, "bundle": "b", "graph": "g"},
        None,
        [],
    )
    assert model.rows() == []


def test_ready_baseline_then_later_rows_overlay_and_earlier_rows_do_not() -> None:
    model = GlobalRuleStatusModel()
    model.observe(
        _OWNER, _catalog(["door", "light"]), _ready({"door": "clear", "light": "clear"}, 10),
        [_row(9, "door", "latched"), _row(11, "light", "tripped")],
    )
    assert model.rows() == [
        {"rule_id": "door", "state": "clear", "keys": []},
        {"rule_id": "light", "state": "tripped", "keys": []},
    ]
    # Pending keeps the qualified cut and keeps following its rows.
    model.observe(
        _OWNER, _catalog(["door", "light"]), {"status": "projection_pending"},
        [_row(12, "door", "latched")],
    )
    assert _states(model) == {"door": "latched", "light": "tripped"}
    # A newer ready cut supersedes rows at or before its boundary.
    model.observe(_OWNER, _catalog(["door", "light"]), _ready({"door": "clear", "light": "clear"}, 12), [])
    assert _states(model) == {"door": "clear", "light": "clear"}


def test_rows_read_before_the_baseline_still_count_after_it() -> None:
    model = GlobalRuleStatusModel()
    model.observe(_OWNER, _catalog(["door"]), {"status": "projection_pending"}, [_row(5, "door", "tripped")])
    assert _states(model) == {"door": "pending"}
    model.observe(_OWNER, _catalog(["door"]), _ready({"door": "clear"}, 4), [])
    assert _states(model) == {"door": "tripped"}


def test_baseline_without_a_proving_catalog_or_null_is_pending() -> None:
    model = GlobalRuleStatusModel()
    model.observe(_OWNER, _catalog(["door"]), _ready({"door": "latched"}, 1, catalog_generation="5"), [])
    assert _states(model) == {"door": "pending"}
    model.observe(_OWNER, _catalog(["door"]), _ready({"door": "latched"}, 1), [])
    assert _states(model) == {"door": "latched"}
    model.observe(_OWNER, _catalog(["door"]), None, [])
    assert _states(model) == {"door": "pending"}
    # Pending without a qualified cut stays pending.
    model.observe(_OWNER, _catalog(["door"]), {"status": "projection_pending"}, [])
    assert _states(model) == {"door": "pending"}


def test_waiting_for_program_rules_name_their_keys() -> None:
    model = GlobalRuleStatusModel()
    model.observe(
        _OWNER,
        {"status": "waiting_for_program", "owner_epoch": _OWNER,
         "active_publication_generation": 3, "bundle": "b", "graph": "g",
         "rule_ids": ["mixed", "other"],
         "waiting_for_program": [{"rule_id": "mixed", "keys": ["part_ready"]},
                                 {"rule_id": "other", "keys": ["a", "b"]}]},
        None,
        [],
    )
    assert model.rows() == [
        {"rule_id": "mixed", "state": "waiting_for_program", "keys": ["part_ready"]},
        {"rule_id": "other", "state": "waiting_for_program", "keys": ["a", "b"]},
    ]
    # The per-rule annotation of an installed catalog wins over its state.
    model.observe(
        _OWNER,
        _catalog(["mixed", "signal"], waiting_for_program=[{"rule_id": "mixed", "keys": ["k"]}]),
        _ready({"mixed": "clear", "signal": "tripped"}, 2),
        [],
    )
    assert _states(model) == {"mixed": "waiting_for_program", "signal": "tripped"}


def test_malformed_wire_is_unknown_and_needs_a_cut_after_the_lost_rows() -> None:
    model = GlobalRuleStatusModel()
    model.observe(_OWNER, _catalog(["door"]), _ready({"door": "clear"}, 1), [])
    bad = _row(7, "door", "exploded")
    with pytest.raises(InterruptStateWireError):
        model.observe(_OWNER, _catalog(["door"]), _ready({"door": "clear"}, 1), [bad])
    assert model.rows() is None
    model.observe(_OWNER, _catalog(["door"]), _ready({"door": "clear"}, 6), [])
    assert _states(model) == {"door": "pending"}
    model.observe(_OWNER, _catalog(["door"]), _ready({"door": "latched"}, 7), [])
    assert _states(model) == {"door": "latched"}


def test_another_owner_is_a_wire_error() -> None:
    model = GlobalRuleStatusModel()
    with pytest.raises(InterruptStateWireError):
        model.observe("kernel-2", _catalog(["door"]), None, [])
    assert model.rows() is None


def test_pending_invalidates_only_notify_until_ready() -> None:
    model = GlobalRuleStatusModel()
    catalog = _catalog(["door", "light"], notify_rule_ids=["door"])
    model.observe(_OWNER, catalog, _ready({"door": "tripped", "light": "latched"}, 0), [])
    model.observe(_OWNER, catalog, {"status": "projection_pending"},
                  [_row(1, "door", "tripped"), _row(2, "light", "clear")])
    assert _states(model) == {"door": "pending", "light": "clear"}
    reason = {
        "code": "motion_interrupt_arm_failed",
        "message": "Notify rule input is unavailable",
        "details": {"reason": "io_rule_source_unavailable", "source": "door"},
    }
    ready = _ready(
        {"door": "unknown", "light": "clear"}, 2,
        unavailable_reasons={"door": reason},
    )
    model.observe(_OWNER, catalog, ready, [])
    assert _states(model) == {"door": "unknown", "light": "clear"}
    assert model.rows() == [
        {"rule_id": "door", "state": "unknown", "keys": [], "unavailable_reason": reason},
        {"rule_id": "light", "state": "clear", "keys": []},
    ]
    model.observe(_OWNER, catalog, _ready({"door": "tripped", "light": "clear"}, 3), [])
    assert _states(model) == {"door": "tripped", "light": "clear"}
    assert model.rows() == [
        {"rule_id": "door", "state": "tripped", "keys": []},
        {"rule_id": "light", "state": "clear", "keys": []},
    ]


def test_unknown_event_reason_is_shown_and_removed_on_fresh_recovery() -> None:
    model = GlobalRuleStatusModel()
    catalog = _catalog(["door"], notify_rule_ids=["door"])
    baseline = _ready({"door": "clear"}, 1)
    model.observe(_OWNER, catalog, baseline, [])
    reason = {
        "code": "motion_interrupt_arm_failed",
        "message": "Notify rule input is unavailable",
        "details": {"reason": "io_rule_source_unavailable"},
    }

    model.observe(
        _OWNER, catalog, baseline,
        [_row(2, "door", "unknown", unavailable_reason=reason)],
    )
    assert model.rows() == [
        {"rule_id": "door", "state": "unknown", "keys": [], "unavailable_reason": reason},
    ]

    model.observe(_OWNER, catalog, baseline, [_row(3, "door", "clear")])
    assert model.rows() == [{"rule_id": "door", "state": "clear", "keys": []}]
