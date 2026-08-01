"""Disposition tests for the removed v0 admission-provider launcher."""

from __future__ import annotations

from manyforge_xarm_adapter import execution_admission_provider_launch as subject


HISTORICAL_TEST_BLOB = "ae5d6c8074ccad8b39c1120a34dfe9c0e52d3507"
HISTORICAL_CASE_DISPOSITION = {
    "test_compiles_exact_provider_command": ("I7/L0", "I8/P5", "I10/R"),
    "test_rejects_missing_exact_provider_fact": (
        "I3",
        "I6/N",
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_rejects_lease_not_longer_than_refresh": (
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_rejects_owner_progress_age_not_longer_than_refresh": (
        "I4/M0",
        "I7/L0",
        "I10/R",
    ),
    "test_rejects_observation_age_not_longer_than_refresh": (
        "I5/H0",
        "I8/P5",
        "I10/R",
    ),
    "test_rejects_unsafe_provider_identity_or_timing": (
        "I6/N",
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
}


def test_launcher_reports_unavailable_before_old_contract_access(capsys) -> None:
    assert subject.main() == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE
    assert not hasattr(subject, "admission_provider_command")


def test_source_and_all_six_historical_case_cohorts_remain_pinned() -> None:
    assert subject.HISTORICAL_SOURCE_BLOB == (
        "79ba4bde9268b907f6e34a48d2301fe80fcf39fe"
    )
    assert HISTORICAL_TEST_BLOB == (
        "ae5d6c8074ccad8b39c1120a34dfe9c0e52d3507"
    )
    assert len(HISTORICAL_CASE_DISPOSITION) == 6
    assert all("I10/R" in gates for gates in HISTORICAL_CASE_DISPOSITION.values())
