"""Dependency-neutral acceptance cases for the non-operative example."""

from __future__ import annotations

from examples.external_nodes.example_plc_write_bool.reference_semantics import (
    WriteBoolEffect,
    WriteBoolAttempt,
    WriteBoolPhase,
    new_write_bool_attempt,
    reduce_write_bool_cancellation,
    reduce_write_bool_tick,
)


def _running_attempt(
    *,
    started_ns: int = 100,
    timeout_ns: int = 1_000,
) -> WriteBoolAttempt:
    decision = reduce_write_bool_tick(
        new_write_bool_attempt(timeout_ns=timeout_ns),
        now_ns=started_ns,
    )
    assert decision.effects == (WriteBoolEffect.SUBMIT_COMMAND,)
    assert decision.attempt.phase is WriteBoolPhase.RUNNING
    return decision.attempt


def test_success() -> None:
    decision = reduce_write_bool_tick(
        _running_attempt(),
        now_ns=101,
        operation_done=True,
        result_ok=True,
    )

    assert decision.attempt.phase is WriteBoolPhase.SUCCEEDED
    assert decision.attempt.diagnostic == ""
    assert decision.effects == ()


def test_provider_failure() -> None:
    decision = reduce_write_bool_tick(
        _running_attempt(),
        now_ns=101,
        operation_done=True,
        result_ok=False,
        result_error="write_rejected",
    )

    assert decision.attempt.phase is WriteBoolPhase.FAILED
    assert decision.attempt.diagnostic == "write_rejected"
    assert decision.effects == ()


def test_timeout_cancels() -> None:
    decision = reduce_write_bool_tick(
        _running_attempt(started_ns=1_000, timeout_ns=10),
        now_ns=1_010,
    )

    assert decision.attempt.phase is WriteBoolPhase.FAILED
    assert decision.attempt.diagnostic == "command_timeout"
    assert decision.effects == (WriteBoolEffect.CANCEL_COMMAND,)


def test_stop_cancels_in_flight_operation() -> None:
    """Historical name retained; this is tree cancellation, not Kernel Stop."""

    decision = reduce_write_bool_cancellation(_running_attempt())

    assert decision.attempt.phase is WriteBoolPhase.CANCELLED
    assert decision.attempt.diagnostic == "command_cancelled"
    assert decision.effects == (WriteBoolEffect.CANCEL_COMMAND,)


def test_completion_at_deadline_is_timeout_without_redundant_cancel() -> None:
    decision = reduce_write_bool_tick(
        _running_attempt(started_ns=5_000, timeout_ns=10),
        now_ns=5_010,
        operation_done=True,
        result_ok=True,
    )

    assert decision.attempt.phase is WriteBoolPhase.FAILED
    assert decision.attempt.diagnostic == "command_timeout"
    assert decision.effects == ()
