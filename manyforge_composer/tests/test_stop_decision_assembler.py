from manyforge_composer.backend.stop_decision_assembler import (
    StopDecisionAssembler,
    StopDecisionBoundary,
)


def _event(
    event_type: str,
    sequence: int,
    *,
    execution_id: int,
    details: dict[str, str] | None = None,
) -> tuple[dict, dict]:
    event = {
        "type": event_type,
        "sequence": sequence,
        "event_index": sequence,
        "execution_id": execution_id,
        "timestamp_ns": sequence * 100,
    }
    normalized = dict(details or {})
    event["details"] = dict(normalized)
    return event, normalized


def test_capacity_eviction_is_explicit_and_identity_bounded():
    assembler = StopDecisionAssembler(capacity=2)
    for execution_id, sequence in ((11, 1), (12, 2)):
        event, details = _event(
            "stop_requested", sequence, execution_id=execution_id
        )
        assert assembler.observe(
            "stop_requested", event, details, run_id="run-capacity"
        ) == []

    event, details = _event("stop_requested", 3, execution_id=13)
    records = assembler.observe(
        "stop_requested", event, details, run_id="run-capacity"
    )

    assert assembler.open_episode_count == 2
    assert len(records) == 1
    evicted = records[0]
    assert evicted.execution_id == 11
    assert evicted.details["chain_complete"] == "false"
    assert evicted.details["completion_boundary"] == "capacity_eviction"
    assert evicted.details["incomplete_reason"] == "assembler_capacity_eviction"


def test_gap_flushes_each_identity_without_cross_episode_merge():
    assembler = StopDecisionAssembler(capacity=4)
    for execution_id, sequence in ((21, 10), (22, 11)):
        event, details = _event(
            "stop_requested", sequence, execution_id=execution_id
        )
        assembler.observe(
            "stop_requested", event, details, run_id="run-gap"
        )

    records = assembler.invalidate_all(
        "kernel_sequence_gap",
        boundary=StopDecisionBoundary.EVENT_GAP,
        missing_sequence_start=12,
        missing_sequence_end=17,
        timestamp_ns=1_800,
    )

    assert assembler.open_episode_count == 0
    assert [record.execution_id for record in records] == [21, 22]
    assert all(record.details["chain_complete"] == "false" for record in records)
    assert all(record.details["missing_sequence_start"] == "12" for record in records)
    assert all(record.details["missing_sequence_end"] == "17" for record in records)


def test_run_end_flush_is_scoped_to_the_requested_run():
    assembler = StopDecisionAssembler(capacity=4)
    for run_id, execution_id, sequence in (
        ("run-a", 31, 20),
        ("run-b", 32, 21),
    ):
        event, details = _event(
            "stop_requested", sequence, execution_id=execution_id
        )
        assembler.observe("stop_requested", event, details, run_id=run_id)

    records = assembler.invalidate_all(
        "run_ended_before_stop_outcome",
        boundary=StopDecisionBoundary.RUN_END,
        timestamp_ns=2_200,
        run_id="run-a",
    )

    assert [record.execution_id for record in records] == [31]
    assert assembler.open_episode_count == 1


def test_conflicting_execution_and_latch_identities_are_fenced_loudly():
    assembler = StopDecisionAssembler(capacity=4)
    first, first_details = _event("stop_requested", 30, execution_id=41)
    assembler.observe("stop_requested", first, first_details, run_id="run-conflict")
    second, second_details = _event(
        "interrupt_latched",
        31,
        execution_id=42,
        details={"latch_id": "latch-conflict", "source_id": "source-42"},
    )
    assembler.observe(
        "interrupt_latched", second, second_details, run_id="run-conflict"
    )

    contradictory, contradictory_details = _event(
        "interrupt_latched",
        32,
        execution_id=41,
        details={"latch_id": "latch-conflict", "source_id": "source-41"},
    )
    records = assembler.observe(
        "interrupt_latched",
        contradictory,
        contradictory_details,
        run_id="run-conflict",
    )

    assert {record.execution_id for record in records} == {41, 42}
    assert all(
        record.details["completion_boundary"] == "identity_conflict"
        for record in records
    )
    assert all(
        record.details["incomplete_reason"]
        == "conflicting_execution_latch_identity"
        for record in records
    )
    assert assembler.open_episode_count == 1
