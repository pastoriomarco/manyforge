"""Cutover gate for the approved I5/H0 alarm catalog owner."""

import pytest

from manyforge_composer.backend.operator_alarm_catalog import (
    OPERATOR_ALARM_CATALOG_REPLACEMENT_GATE,
    OperatorAlarmCatalogUnavailableError,
    load_operator_alarm_catalog,
)


def test_catalog_loader_fails_before_source_access() -> None:
    class PoisonSource:
        def __getattribute__(self, name: str) -> object:
            raise AssertionError(f"catalog source was accessed: {name}")

    with pytest.raises(
        OperatorAlarmCatalogUnavailableError,
        match="composable_cell_operator_alarm_catalog_unavailable",
    ):
        load_operator_alarm_catalog(PoisonSource())

    assert OPERATOR_ALARM_CATALOG_REPLACEMENT_GATE == "I5/H0"
