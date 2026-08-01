"""Cutover gate for source-owned I5/H0 alarm condition producers."""

import pytest

from manyforge_composer.backend.operator_alarm_adapters import (
    OPERATOR_ALARM_ADAPTERS_RESTORATION_GATES,
    OperatorAlarmAdapters,
    OperatorAlarmAdaptersUnavailableError,
)


def test_adapters_fail_before_manager_or_collaborator_access() -> None:
    class PoisonCollaborator:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"alarm collaborator was accessed: {name}")

    poison = PoisonCollaborator()
    with pytest.raises(
        OperatorAlarmAdaptersUnavailableError,
        match="composable_cell_operator_alarm_adapters_unavailable",
    ):
        OperatorAlarmAdapters(
            poison,
            lifecycle_sink=poison,
            monotonic_clock=poison,
        )

    assert OPERATOR_ALARM_ADAPTERS_RESTORATION_GATES == ("I5", "H0")
