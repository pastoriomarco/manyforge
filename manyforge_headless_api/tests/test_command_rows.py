"""The command-row table of commands answered at acceptance (OD-214)."""

from __future__ import annotations

import itertools

import pytest

from manyforge_headless_api.command_rows import CommandRowTable
from manyforge_headless_api.program_cycle import (
    MAXIMUM_ENDED_COMMAND_ROWS,
    RunnerCommandsDTO,
)


def _table() -> tuple[CommandRowTable, list[None]]:
    changes: list[None] = []
    clock = itertools.count(1_763_112_345_678_901_234)
    table = CommandRowTable(on_change=lambda: changes.append(None), clock_ns=lambda: next(clock))
    return table, changes


def test_a_row_is_accepted_steps_and_ends_in_the_runner_row_shape() -> None:
    table, changes = _table()
    key = "a" * 32

    accepted = table.accept("start", key, step="admitting")
    table.step(key, "claiming")
    ended = table.end(key, "failed", "program_run_started_then_failed", {"program_run_id": 3})

    assert accepted == {
        "kind": "start", "request_key": key, "step": "admitting",
        "accepted_at_ns": "1763112345678901234", "end": None,
    }
    assert ended == {
        "kind": "start", "request_key": key, "step": "claiming",
        "accepted_at_ns": "1763112345678901234",
        "end": {"outcome": "failed", "code": "program_run_started_then_failed",
                "details": {"program_run_id": 3}, "finished_at_ns": "1763112345678901235"},
    }
    snapshot = table.snapshot()
    assert snapshot == {"in_progress": None, "ended": [ended]}
    # The same shape the runner publishes, validated by its DTO.
    RunnerCommandsDTO.model_validate(snapshot)
    # Every acceptance, step and end wakes the observer once.
    assert len(changes) == 3


def test_a_refusal_before_acceptance_is_an_ended_row_never_accepted() -> None:
    table, _changes = _table()
    row = table.refuse("start", "b" * 32, "apply_in_progress", None, step_source=None)
    assert row["accepted_at_ns"] is None and row["step"] is None
    assert row["end"]["outcome"] == "refused" and row["end"]["details"] is None
    assert row["step_source"] is None
    assert table.find("b" * 32) == row


def test_a_kept_key_is_found_as_it_now_stands() -> None:
    table, _changes = _table()
    table.accept("start", "c" * 32, step="admitting", captured_input_digest="d")
    table.update("c" * 32, captured_input_digest="e")
    assert table.find("c" * 32)["captured_input_digest"] == "e"
    assert table.find("d" * 32) is None
    # A copy: the caller cannot change the kept row.
    table.find("c" * 32)["step"] = "x"
    assert table.find("c" * 32)["step"] == "admitting"


def test_one_command_at_a_time_and_only_the_last_ended_rows_are_kept() -> None:
    table, _changes = _table()
    table.accept("start", "0" * 32)
    with pytest.raises(RuntimeError):
        table.accept("start", "1" * 32)
    table.end("0" * 32, "succeeded")
    for index in range(1, MAXIMUM_ENDED_COMMAND_ROWS + 2):
        key = f"{index:032x}"
        table.accept("start", key)
        table.end(key, "succeeded")
    ended = table.snapshot()["ended"]
    assert len(ended) == MAXIMUM_ENDED_COMMAND_ROWS
    # Newest first; the oldest are forgotten.
    assert ended[0]["request_key"] == f"{MAXIMUM_ENDED_COMMAND_ROWS + 1:032x}"
    assert table.find("0" * 32) is None


def test_an_end_has_a_code_exactly_when_it_did_not_succeed() -> None:
    table, _changes = _table()
    table.accept("start", "e" * 32)
    with pytest.raises(ValueError):
        table.end("e" * 32, "succeeded", "x")
    with pytest.raises(ValueError):
        table.end("e" * 32, "failed")
    # A succeeded Apply-level end keeps its component rows (contract §3.1).
    assert table.end("e" * 32, "succeeded", None, {"components": []})["end"]["details"] == {
        "components": []
    }


def test_an_untyped_failure_before_acceptance_ends_unknown() -> None:
    table, _changes = _table()
    row = table.refuse(
        "scenario_load", "f" * 32, "program_runtime_control_outcome_unknown",
        {"command_kind": "scenario_load", "cause_code": "KeyError"}, outcome="unknown",
    )
    assert (row["accepted_at_ns"], row["step"]) == (None, None)
    assert row["end"]["outcome"] == "unknown"
