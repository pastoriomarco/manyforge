"""Bounded immutable alarm-history coverage and the legacy-owner fence."""

from collections.abc import Iterator, Mapping
from typing import Any

import pytest

from manyforge_composer.backend import operator_alarms
from manyforge_composer.backend.operator_alarms import (
    OPERATOR_ALARM_OWNER_REPLACEMENT_GATE,
    AlarmLifecycleFact,
    OperatorAlarmManager,
    OperatorAlarmOwnerUnavailableError,
)


def _fact(**overrides: Any) -> AlarmLifecycleFact:
    values: dict[str, Any] = {
        "event_type": "operator_alarm_activated",
        "definition_id": "definition-1",
        "condition_id": "condition-1",
        "activation_id": "activation-1",
        "previous_state": None,
        "current_state": "active_unacknowledged",
        "record_revision": 1,
        "manager_generation": 1,
        "occurred_at_ns": 100,
        "occurred_at_wall": "2026-07-31T00:00:00Z",
    }
    values.update(overrides)
    return AlarmLifecycleFact(**values)


def test_composer_exports_no_alarm_lifecycle_reducer() -> None:
    for symbol in (
        "AlarmConditionUpdate",
        "AlarmPointState",
        "AlarmTransitionResult",
        "OperatorAlarmMutation",
        "AlarmAcknowledgementMember",
        "OperatorAlarmRecord",
        "canonical_condition_id",
        "reduce_condition_update",
        "reduce_acknowledgement",
    ):
        assert not hasattr(operator_alarms, symbol)


def test_history_fact_copies_inputs_and_preserves_public_event_shape() -> None:
    correlations = {"run_id": {"parts": ["run-1"]}}
    details = {"trace_id": "trace-1"}
    fact = _fact(
        correlations=correlations,
        details=details,
    )
    correlations["run_id"]["parts"][0] = "mutated"
    details["trace_id"] = "mutated"

    assert fact.correlations["run_id"]["parts"] == ("run-1",)
    assert fact.details["trace_id"] == "trace-1"
    with pytest.raises(TypeError):
        fact.correlations["new"] = "value"  # type: ignore[index]
    event = fact.to_event_details()
    assert event == {
        "definition_id": "definition-1",
        "condition_id": "condition-1",
        "activation_id": "activation-1",
        "previous_state": None,
        "current_state": "active_unacknowledged",
        "record_revision": 1,
        "manager_generation": 1,
        "source_id": None,
        "source_epoch": None,
        "source_sequence": None,
        "principal_id": None,
        "reason": None,
        "occurred_at_ns": 100,
        "occurred_at": "2026-07-31T00:00:00Z",
        "correlations": {"run_id": {"parts": ["run-1"]}},
        "trace_id": "trace-1",
    }
    event["correlations"]["run_id"]["parts"][0] = "caller mutation"
    assert fact.correlations["run_id"]["parts"] == ("run-1",)


def test_history_fact_rejects_non_builtin_mappings_without_access() -> None:
    class PoisonMapping(Mapping[str, Any]):
        def __getitem__(self, key: str) -> Any:
            raise AssertionError(f"mapping lookup attempted: {key}")

        def __iter__(self) -> Iterator[str]:
            raise AssertionError("mapping iteration attempted")

        def __len__(self) -> int:
            raise AssertionError("mapping length attempted")

    with pytest.raises(TypeError, match="built-in mapping"):
        _fact(details=PoisonMapping())


def test_history_fact_rejects_keys_and_values_without_stringification() -> None:
    class Poison:
        def __hash__(self) -> int:
            return 7

        def __str__(self) -> str:
            raise AssertionError("arbitrary stringification attempted")

    with pytest.raises(TypeError, match="non-string mapping key"):
        _fact(details={Poison(): "value"})
    with pytest.raises(TypeError, match="non-JSON value"):
        _fact(details={"value": Poison()})


def test_history_fact_rejects_cycles_depth_and_collection_overflow() -> None:
    cyclic: dict[str, Any] = {}
    cyclic["self"] = cyclic
    with pytest.raises(ValueError, match="container cycle"):
        _fact(details=cyclic)

    deep: dict[str, Any] = {}
    cursor = deep
    for _ in range(10):
        child: dict[str, Any] = {}
        cursor["child"] = child
        cursor = child
    with pytest.raises(ValueError, match="nesting depth"):
        _fact(details=deep)

    with pytest.raises(ValueError, match="exceeds 64 mapping entries"):
        _fact(details={str(index): index for index in range(65)})
    with pytest.raises(ValueError, match="exceeds 256 sequence entries"):
        _fact(details={"items": list(range(257))})
    with pytest.raises(ValueError, match="bounded value node count"):
        _fact(details={"rows": [[0] * 256 for _ in range(17)]})


def test_history_fact_rejects_unbounded_or_ambiguous_scalars() -> None:
    with pytest.raises(TypeError, match="non-negative integer"):
        _fact(manager_generation=True)
    with pytest.raises(ValueError, match="must be non-negative"):
        _fact(record_revision=-1)
    with pytest.raises(ValueError, match="oversized integer"):
        _fact(details={"value": 1 << 65})
    with pytest.raises(ValueError, match="non-finite"):
        _fact(details={"value": float("inf")})
    with pytest.raises(ValueError, match="Unicode NFC"):
        _fact(details={"value": "e\u0301"})
    with pytest.raises(ValueError, match="32768 UTF-8 bytes"):
        _fact(details={"value": "x" * (32 * 1024 + 1)})
    with pytest.raises(ValueError, match="total text bound"):
        _fact(details={"values": ["x" * (32 * 1024) for _ in range(9)]})
    with pytest.raises(TypeError, match="event_type must be a non-empty string"):
        _fact(event_type=object())
    with pytest.raises(TypeError, match="definition_id must be a non-empty string"):
        _fact(definition_id=object())


def test_history_details_cannot_replace_canonical_fields() -> None:
    with pytest.raises(ValueError, match="cannot replace canonical"):
        _fact(details={"condition_id": "forged-condition"})


def test_manager_fails_before_catalog_or_collaborator_access() -> None:
    class PoisonCollaborator:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"alarm collaborator was accessed: {name}")

    poison = PoisonCollaborator()
    with pytest.raises(
        OperatorAlarmOwnerUnavailableError,
        match="composable_cell_operator_alarm_owner_unavailable",
    ):
        OperatorAlarmManager(poison, history_sink=poison)

    assert OPERATOR_ALARM_OWNER_REPLACEMENT_GATE == "I5/H0"
