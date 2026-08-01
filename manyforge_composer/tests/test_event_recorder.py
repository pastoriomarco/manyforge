"""Focused tests for the retained event-history presentation primitives.

Historical acceptance disposition
---------------------------------

The 142-case suite at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` remains the exact
acceptance source.  Its groups move as follows:

* retained here: the pure filtering/normalized-export portions of
  ``TestJSONLSink``, the fixed-bucket cases
  ``TestLatencyHistograms.{test_bucket_assignment_and_stats_golden,
  test_percentile_interpolation_golden,test_overflow_negative_and_empty_edges}``,
  and wall-window value behavior from ``TestCrossRunHistory``;
* I5/H0 Historian: the four top-level alarm/lifecycle durability cases,
  ``TestRunLifecycle``, ``TestObserveConcurrency``, ``TestJSONLSink`` storage,
  ``TestBoundedRetention``, ``TestGetEventsSince``, ``TestDurableFaultWrites``,
  ``TestClockAnchor``, ``TestKernelFlightDumpIngest``,
  ``TestKernelEventPipelineLossVisibility``, ``TestTraceRetentionAndRehydration``,
  ``TestOW5DurabilityAndShutdown``, and storage/index/pagination portions of
  ``TestCrossRunHistory``;
* I6/N source owners plus I5/H0 ingestion: ``TestSemanticEvents``,
  ``TestSynthesizedSemanticEvents`` (replaced by source-issued facts, never
  Composer inference), ``TestProgramRunIdAdoption``, and non-Stop latency
  attribution cases;
* I7/L0 source owners plus I5/H0 ingestion: controlled-component lifecycle
  and route facts; and
* I8/P5 Kernel/Stop owner plus I5/H0 ingestion:
  ``TestStopDecisionChains``, ``TestRecoveryEvidenceCorrelation``, Stop-latency
  attribution, and causal incident portions of ``TestEventChannelsAndIncidents``.

``TestInterventionProjection`` and the non-causal bounded-view portions of
``TestEventChannelsAndIncidents`` split between the source gate that emits the
fact and I5/H0 Historian retention.  No historical case is evidence for the
temporary Composer shell.
"""

from __future__ import annotations

import pytest

from manyforge_composer.backend import event_recorder
from manyforge_composer.backend.event_recorder import (
    HISTORIAN_RESTORATION_GATES,
    HISTORY_SOURCE_REQUALIFICATION_GATES,
    EventPageBoundsError,
    EventRecorder,
    HistorianClientUnavailableError,
    HistoryQuery,
    LatencyHistogram,
    MAX_HISTORY_PAGE_LIMIT,
    filter_history_events,
    parse_wall_timestamp_ns,
    project_history_page,
    project_normalized_trace,
    require_historian_client,
)


class _PoisonInput:
    def __getattribute__(self, name: str) -> object:
        raise AssertionError(f"historian gate accessed former input: {name}")

    def __iter__(self):
        raise AssertionError("historian gate iterated former input")


def _event(
    index: int,
    *,
    event_type: str = "execute_completed",
    severity: str = "INFO",
    component: str = "manyforge.execution",
    execution_id: int = 7,
    timestamp_wall: str = "2026-07-31T10:00:00Z",
    **extra: object,
) -> dict[str, object]:
    return {
        "event_index": index,
        "timestamp_ns": index * 100,
        "timestamp_wall": timestamp_wall,
        "type": event_type,
        "severity": severity,
        "component": component,
        "execution_id": execution_id,
        "message": f"event {index}",
        **extra,
    }


def test_recorder_gate_fails_before_state_storage_or_event_access() -> None:
    poison = _PoisonInput()
    with pytest.raises(
        HistorianClientUnavailableError,
        match="composable_cell_historian_client_unavailable",
    ):
        EventRecorder(poison, trace_dir=poison, state=poison)

    with pytest.raises(HistorianClientUnavailableError):
        require_historian_client(poison, storage=poison)

    assert HISTORIAN_RESTORATION_GATES == ("I5", "H0")
    assert HISTORY_SOURCE_REQUALIFICATION_GATES == (
        "I6",
        "N",
        "I7",
        "L0",
        "I8",
        "P5",
    )


def test_filtering_is_bounded_and_preserves_the_recent_matching_tail() -> None:
    events = [
        _event(1, severity="DEBUG"),
        _event(
            2,
            severity="WARNING",
            component="manyforge.execution.backend",
            details={"action_span_id": "span-1", "move_id": "move-2"},
        ),
        _event(
            3,
            severity="ERROR",
            component="manyforge.execution",
            details={"action_span_id": "span-1", "move_id": "move-3"},
        ),
        _event(
            4,
            severity="CRITICAL",
            component="manyforge.scene",
            details={"action_span_id": "span-1"},
        ),
    ]
    query = HistoryQuery(
        min_severity="warn",
        components=("manyforge.execution",),
        event_types=("execute_completed",),
        execution_id=7,
        action_span_id="span-1",
    )

    selected = filter_history_events(events, query=query, limit=1)

    assert [event["event_index"] for event in selected] == [3]
    assert selected[0] is not events[2]
    assert selected[0]["details"] is not events[2]["details"]

    selected_by_move = filter_history_events(
        events,
        query=HistoryQuery(move_id="move-2"),
        limit=4,
    )
    assert [event["event_index"] for event in selected_by_move] == [2]
    assert filter_history_events(
        events,
        query=HistoryQuery(move_id="move-missing"),
        limit=4,
    ) == []


def test_wall_filter_uses_integer_time_and_keeps_unparseable_evidence() -> None:
    lower = parse_wall_timestamp_ns("2026-07-31T10:00:00.1Z")
    upper = parse_wall_timestamp_ns("2026-07-31T10:00:00.9+00:00")
    assert lower is not None
    assert upper is not None
    assert lower < upper

    events = [
        _event(1, timestamp_wall="2026-07-31T09:59:59Z"),
        _event(2, timestamp_wall="2026-07-31T10:00:00.5Z"),
        _event(3, timestamp_wall="not-a-wall-time"),
    ]

    selected = filter_history_events(
        events,
        query=HistoryQuery(from_wall_ns=lower, to_wall_ns=upper),
    )

    assert [event["event_index"] for event in selected] == [2, 3]


def test_filter_rejects_invalid_or_oversized_client_pages() -> None:
    with pytest.raises(ValueError, match="unsupported severity"):
        HistoryQuery(min_severity="notice")
    with pytest.raises(ValueError, match="must agree"):
        HistoryQuery(execution_id=1, kernel_execution_id=2)

    oversized = (_event(index) for index in range(MAX_HISTORY_PAGE_LIMIT + 1))
    with pytest.raises(EventPageBoundsError, match="exceeds"):
        filter_history_events(oversized, limit=MAX_HISTORY_PAGE_LIMIT)


def test_history_projection_splits_node_transitions_without_owning_state() -> None:
    events = [
        _event(1, execution_id=3),
        _event(
            2,
            event_type="node_status_transition",
            execution_id=4,
            node="move-a",
            **{"from": "running", "to": "success"},
        ),
    ]

    page = project_history_page(
        events,
        observed_at_ns=999,
        selected_run_id="run-1",
    )

    assert [event["event_index"] for event in page["events"]] == [1]
    assert page["node_status_transitions"] == [
        {
            "timestamp_ns": 200,
            "timestamp": None,
            "run_id": None,
            "program_run_id": None,
            "intervention_run_id": None,
            "session_id": None,
            "session_kind": None,
            "authority_source": None,
            "principal_id": None,
            "grant_type": None,
            "parent_run_id": None,
            "trigger_reason": None,
            "trigger_node_instance": None,
            "execution_mode": None,
            "execution_id": 4,
            "node": "move-a",
            "from": "running",
            "to": "success",
        }
    ]
    assert page["execution_id"] == 4
    assert page["timestamp_ns"] == 999


def test_normalized_trace_projection_is_explicit_pure_and_bounded() -> None:
    source_event = _event(
        9,
        run_id="run-1",
        program_run_id="run-1",
        intervention_run_id="intervention-2",
        parent_run_id="run-1",
        operation_id="operation-3",
        operation_revision=4,
        details={"move_id": "move-5", "custom": "kept"},
    )

    trace = project_normalized_trace(
        [source_event],
        exported_at_ns=1234,
        trace_source="manyforge.historian",
        run_id="run-1",
    )

    assert trace["schema_version"] == "manyforge.ros2_medkit_trace.v1"
    assert trace["trace_source"] == "manyforge.historian"
    assert trace["exported_at_ns"] == 1234
    assert trace["intervention_run_ids"] == ["intervention-2"]
    assert trace["parent_run_ids"] == ["run-1"]
    assert trace["records"][0]["record_id"] == "intervention-2:9"
    assert trace["records"][0]["operation_id"] == "operation-3"
    assert trace["records"][0]["move_id"] == "move-5"
    assert trace["records"][0]["attributes"] == {
        "move_id": "move-5",
        "custom": "kept",
    }
    assert source_event["details"] == {"move_id": "move-5", "custom": "kept"}


def test_fixed_bucket_latency_summary_retains_historical_value_behavior() -> None:
    histogram = LatencyHistogram()
    assert histogram.percentile_ns(0.95) is None

    histogram.observe(-1)
    histogram.observe(1_000_000)
    histogram.observe(2_000_000)
    histogram.observe(11_000_000_000)

    payload = histogram.to_payload()
    assert payload["count"] == 3
    assert payload["sum_ns"] == 11_003_000_000
    assert payload["min_ns"] == 1_000_000
    assert payload["max_ns"] == 11_000_000_000
    assert payload["bucket_counts"][0] == 1
    assert payload["bucket_counts"][1] == 1
    assert payload["bucket_counts"][-1] == 1
    assert histogram.percentile_ns(0.5) == 1_500_000
    assert histogram.percentile_ns(0.95) == 10_000_000_000


def test_old_semantic_and_storage_owners_are_absent() -> None:
    assert not hasattr(EventRecorder, "observe")
    assert not hasattr(EventRecorder, "history")
    assert not hasattr(event_recorder, "_stable_recovery_context_id")
    assert not hasattr(event_recorder, "_stable_stop_decision_id")
    assert not hasattr(event_recorder, "StopDecisionAssembler")
    assert "threading" not in event_recorder.__dict__
    assert "Path" not in event_recorder.__dict__
