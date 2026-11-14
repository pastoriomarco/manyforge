"""Headless GUI projection of the Kernel's global rule states."""

from __future__ import annotations

from typing import Any

import pytest

from manyforge_headless_api.global_rule_status import (
    GlobalRuleStatusModel,
    InterruptStateWireError,
    parse_baseline,
)

_CONTEXT = {
    "catalog_generation": "4",
    "active_publication_generation": 3,
    "retained_bundle_id": "bundle-1",
    "graph_revision": "graph-1",
}


def _catalog(rule_ids: list[str], **extra: Any) -> dict[str, Any]:
    return {
            "catalog_generation": _CONTEXT["catalog_generation"],
        "active_publication_generation": 3,
        "bundle": "bundle-1",
        "graph": "graph-1",
        "rule_ids": rule_ids,
        "referenced_program_blackboard_keys": [],
        **extra,
    }


def _ready(
    states: dict[str, str],
    unavailable_reasons: dict[str, dict[str, Any]] | None = None,
    **context: Any,
) -> dict[str, Any]:
    return {
        "status": "ready",
        "snapshot": {
            **_CONTEXT,
            **context,
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


def _states(model: GlobalRuleStatusModel) -> dict[str, str] | None:
    rows = model.rows()
    return None if rows is None else {row["rule_id"]: row["state"] for row in rows}


def test_unknown_catalog_is_unknown_and_verified_empty_is_no_rules() -> None:
    model = GlobalRuleStatusModel()
    assert model.rows() is None
    model.observe(None, None)
    assert model.rows() is None
    model.observe(
        {"status": "verified_empty", "active_publication_generation": 3, "bundle": "b", "graph": "g"},
        None,
    )
    assert model.rows() == []


def test_each_ready_baseline_is_the_current_state() -> None:
    # No journal row is read: a slow or absent journal changes nothing.
    model = GlobalRuleStatusModel()
    model.observe(_catalog(["door", "light"]), _ready({"door": "clear", "light": "tripped"}))
    assert model.rows() == [
        {"rule_id": "door", "state": "clear", "keys": []},
        {"rule_id": "light", "state": "tripped", "keys": []},
    ]
    model.observe(_catalog(["door", "light"]), _ready({"door": "inactive", "light": "clear"}))
    assert _states(model) == {"door": "inactive", "light": "clear"}


def test_a_trip_inside_one_interval_leaves_the_panel_unchanged() -> None:
    model = GlobalRuleStatusModel()
    model.observe(_catalog(["door"]), _ready({"door": "clear"}))
    # Trip and clear happened between two pages: both carry clear.
    model.observe(_catalog(["door"]), _ready({"door": "clear"}))
    assert _states(model) == {"door": "clear"}


def test_baseline_without_a_proving_catalog_or_null_is_pending() -> None:
    model = GlobalRuleStatusModel()
    model.observe(_catalog(["door"]), _ready({"door": "inactive"}, catalog_generation="5"))
    assert _states(model) == {"door": "pending"}
    model.observe(_catalog(["door"]), _ready({"door": "inactive"}))
    assert _states(model) == {"door": "inactive"}
    model.observe(_catalog(["door"]), None)
    assert _states(model) == {"door": "pending"}
    # A rule of the catalog waits for a Program: no state is known.
    model.observe(_catalog(["door"]), {"status": "projection_pending"})
    assert _states(model) == {"door": "pending"}


def test_a_ready_cut_field_is_refused() -> None:
    ready = _ready({"door": "clear"})
    ready["snapshot"]["through_sequence"] = 0
    with pytest.raises(InterruptStateWireError):
        parse_baseline(ready)


def test_waiting_for_program_rules_name_their_keys() -> None:
    model = GlobalRuleStatusModel()
    model.observe(
        {"status": "waiting_for_program", "active_publication_generation": 3, "bundle": "b", "graph": "g",
         "rule_ids": ["mixed", "other"],
         "waiting_for_program": [{"rule_id": "mixed", "keys": ["part_ready"]},
                                 {"rule_id": "other", "keys": ["a", "b"]}]},
        None,
    )
    assert model.rows() == [
        {"rule_id": "mixed", "state": "waiting_for_program", "keys": ["part_ready"]},
        {"rule_id": "other", "state": "waiting_for_program", "keys": ["a", "b"]},
    ]
    # The per-rule annotation of an installed catalog wins over its state.
    model.observe(
        _catalog(["mixed", "signal"], waiting_for_program=[{"rule_id": "mixed", "keys": ["k"]}]),
        _ready({"mixed": "clear", "signal": "tripped"}),
    )
    assert _states(model) == {"mixed": "waiting_for_program", "signal": "tripped"}


def test_malformed_wire_is_unknown_until_the_next_readable_baseline() -> None:
    model = GlobalRuleStatusModel()
    model.observe(_catalog(["door"]), _ready({"door": "clear"}))
    with pytest.raises(InterruptStateWireError):
        model.observe(_catalog(["door"]), _ready({"door": "exploded"}))
    assert model.rows() is None
    model.observe(_catalog(["door"]), _ready({"door": "inactive"}))
    assert _states(model) == {"door": "inactive"}


def test_notify_and_stop_rules_follow_the_baseline_alike() -> None:
    model = GlobalRuleStatusModel()
    catalog = _catalog(["door", "light"])
    model.observe(catalog, _ready({"door": "tripped", "light": "inactive"}))
    assert _states(model) == {"door": "tripped", "light": "inactive"}
    reason = {
        "code": "motion_interrupt_arm_failed",
        "message": "Notify rule input is unavailable",
        "details": {"reason": "io_rule_source_unavailable", "source": "door"},
    }
    model.observe(catalog, _ready({"door": "unknown", "light": "clear"}, unavailable_reasons={"door": reason}))
    assert model.rows() == [
        {"rule_id": "door", "state": "unknown", "keys": [], "unavailable_reason": reason},
        {"rule_id": "light", "state": "clear", "keys": []},
    ]
    # A fresh reading removes the reason.
    model.observe(catalog, _ready({"door": "tripped", "light": "clear"}))
    assert model.rows() == [
        {"rule_id": "door", "state": "tripped", "keys": []},
        {"rule_id": "light", "state": "clear", "keys": []},
    ]


def test_rule_states_are_the_contract_states_and_latched_is_gone() -> None:
    from manyforge_headless_api.global_rule_status import (
        RULE_STATES,
        InterruptStateWireError,
        parse_baseline,
    )

    assert RULE_STATES == {"inactive", "clear", "tripped", "unknown"}
    with pytest.raises(InterruptStateWireError):
        parse_baseline(_ready({"door": "latched"}))
