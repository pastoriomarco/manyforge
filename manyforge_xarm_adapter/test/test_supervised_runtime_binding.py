"""Disposition tests for the removed v0 supervised-runtime resolver."""

from __future__ import annotations

from manyforge_xarm_adapter import supervised_runtime_binding as subject
import pytest


HISTORICAL_TEST_BLOB = "9abbb0d1321a7ec1ff9a37da814331386ab099bc"
HISTORICAL_CASE_DISPOSITION = {
    "test_resolves_only_runtime_identity_and_preserves_binding": (
        "I3",
        "I6/N",
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_rejects_unqualified_runtime_values": ("I3", "I6/N", "I10/R"),
    "test_rejects_template_drift_before_resolution": ("I3", "I10/R"),
}


def test_old_resolver_is_unavailable_before_any_input_access(capsys) -> None:
    assert subject.main() == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE
    assert not hasattr(subject, "resolve_supervised_runtime_binding")
    assert not hasattr(subject, "RUNTIME_BINDING_SCHEMA")
    assert not hasattr(subject, "EXECUTION_ADMISSION_SCHEMA")


def test_literal_usable_unicast_address_validation_remains_effect_free() -> None:
    assert subject.validate_robot_ipv4("192.168.1.31") == "192.168.1.31"


@pytest.mark.parametrize(
    "value",
    [
        "not-an-address",
        "0.0.0.0",
        "0.1.2.3",
        "127.0.0.1",
        "224.0.0.1",
        "255.255.255.255",
    ],
)
def test_unqualified_historical_address_rows_remain_rejected(value: str) -> None:
    with pytest.raises(ValueError):
        subject.validate_robot_ipv4(value)


def test_source_and_all_three_historical_cohorts_remain_pinned() -> None:
    assert subject.HISTORICAL_SOURCE_BLOB == (
        "7d72defbfb7e6cf394be02891f08fc6c921e1068"
    )
    assert HISTORICAL_TEST_BLOB == (
        "9abbb0d1321a7ec1ff9a37da814331386ab099bc"
    )
    assert len(HISTORICAL_CASE_DISPOSITION) == 3
    assert all("I10/R" in gates for gates in HISTORICAL_CASE_DISPOSITION.values())
