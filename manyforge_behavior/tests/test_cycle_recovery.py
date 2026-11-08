from __future__ import annotations

import json

import pytest

from manyforge_behavior.cycle_recovery import (
    CycleRecoveryCoordinator,
    CycleRecoveryOwnerUnavailableError,
)
from manyforge_behavior.runtime_pause import RuntimePauseRegistry
from manyforge_behavior.runtime_pause import (
    RuntimePauseOwnerUnavailableError,
    sanitize_runtime_diagnostics,
)


def test_v1_recovery_coordinator_fails_before_inputs_or_callbacks() -> None:
    class PoisonCollaborator:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"recovery collaborator was accessed: {name}")

    poison = PoisonCollaborator()
    with pytest.raises(
        CycleRecoveryOwnerUnavailableError,
        match="composable_cell_cycle_recovery_owner_unavailable",
    ):
        CycleRecoveryCoordinator(on_offer_published=poison, kernel=poison)


def test_diagnostic_sanitizer_bounds_and_json_normalizes_nested_values():
    cyclic: dict[str, object] = {}
    cyclic["self"] = cyclic

    details = sanitize_runtime_diagnostics(
        {
            "large": "x" * 20_000,
            "deep": {"a": {"b": {"c": {"d": {"e": {"f": 1}}}}}},
            "cycle": cyclic,
            "non_finite": float("nan"),
            "opaque": object(),
            **{f"key-{index}": index for index in range(80)},
        },
    )

    assert len(details) <= 64
    assert details["_manyforge_details_truncated"] is True
    assert len(details["large"]) == 1024
    assert "<max_depth>" in json.dumps(details["deep"])
    assert details["cycle"]["self"] == "<cycle>"
    assert details["non_finite"] == "<non_finite:nan>"
    assert details["opaque"].startswith("<unsupported:")
    assert json.loads(json.dumps(details, allow_nan=False)) == details
    assert len(json.dumps(details)) < 20_000


def test_runtime_pause_registry_fails_before_allocating_live_state() -> None:
    with pytest.raises(
        RuntimePauseOwnerUnavailableError,
        match="composable_cell_runtime_pause_owner_unavailable",
    ):
        RuntimePauseRegistry()
