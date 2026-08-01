"""Disposition tests for the removed attended-command evidence runner."""

from __future__ import annotations

from manyforge_xarm_adapter import supervised_command_evidence as subject


HISTORICAL_TEST_BLOB = "56f15e21dfb8886027577f4046f70d4ce8fb23eb"
HISTORICAL_CASE_DISPOSITION = {
    "test_capture_binds_runtime_action_and_exact_confirmation": (
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_timeout_is_a_failed_command_evidence_gate": ("I8/P5", "I10/R"),
    "test_mismatched_inner_command_identity_fails_closed": (
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_mismatched_confirmation_evidence_fails_closed": (
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_external_authority_binding_is_rejected": ("I7/L0", "I10/R"),
    "test_enable_command_captures_unadjudicated_transient": (
        "I7/L0",
        "I8/P5",
        "I10/R",
    ),
    "test_incomplete_enable_transient_fails_evidence": ("I8/P5", "I10/R"),
}


def test_command_runner_fails_before_old_session_or_command_access(capsys) -> None:
    assert subject.main() == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE
    assert not hasattr(subject, "capture_supervised_command_evidence")
    assert not hasattr(subject, "COMMAND_RESULT_SCHEMA")
    assert not hasattr(subject, "CommandOutcome")


def test_source_and_all_seven_historical_cohorts_remain_pinned() -> None:
    assert subject.HISTORICAL_SOURCE_BLOB == (
        "cabaf2b1e0f15972a39563e0357c7b02418d5c0c"
    )
    assert HISTORICAL_TEST_BLOB == (
        "56f15e21dfb8886027577f4046f70d4ce8fb23eb"
    )
    assert len(HISTORICAL_CASE_DISPOSITION) == 7
    assert all("I10/R" in gates for gates in HISTORICAL_CASE_DISPOSITION.values())
