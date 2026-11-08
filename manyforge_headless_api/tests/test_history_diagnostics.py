"""On-demand diagnostics never infer missing effects or compare foreign clocks."""

from copy import deepcopy
import hashlib

import pytest
import rfc8785
from pydantic import ValidationError

from manyforge_headless_api.history_diagnostics import (
    causal_references,
    forensic_bundle,
)
from manyforge_headless_api.history_query import (
    HistorianDebugRequest,
    HistorianDiscoveryRequest,
    HistorianQueryRequest,
)
from manyforge_headless_api.service import _history_page
from manyforge_headless_api.native import HeadlessApiError


def record(
    sequence,
    *,
    state="executing",
    kind="operation_state_updated",
    clock="kernel.1",
    time_ns=100,
    operation="op.1",
    revision=1,
):
    return {
        "sequence": sequence,
        "clock_owner": clock,
        "occurred_at_utc_ns": "1790000000000000000",
        "wall_clock_regressed": False,
        "condition_severity": "INFO",
        "persistence_digest": "sha256:" + "a" * 64,
        "event": {
            "schema_version": "manyforge.historian_event.v2",
            "event_id": f"event.{sequence}",
            "component": "manyforge.runtime",
            "event_type": kind,
            "severity": "INFO",
            "occurred_at_ns": str(time_ns),
            "occurred_at_wall": "2026-09-06T12:00:00Z",
            "payload": {
                "schema_version": "manyforge.operation_history_event.v1",
                "operation_id": operation,
                "native_sequence": sequence,
                "operation_revision": revision,
                "record_kind": kind,
                "journal_payload": {"operation_state": {"state": state, "program_run_id": "run.1"}},
            },
        },
    }


def test_run_latency_pairs_exact_owner_clocks_and_retains_full_causal_facts():
    base = 31 * 365 * 86400 * 1_000_000_000
    events = [
        record(1, kind="operation_accepted", state="queued", time_ns=base),
        record(2, time_ns=base + 3),
        record(3, time_ns=base + 11, state="completed"),
        record(4, clock="kernel.restarted", state="completed", time_ns=2),
    ]
    payload = events[2]["event"]["payload"]
    payload["journal_payload"]["blackboard"] = {
        "grasp_confirmed": {"value": True, "attempt_id": "attempt.1"}
    }
    payload["references"] = {
        "scene_id": "scene.at.incident",
        "parent_program_run_id": "parent.1",
    }
    payload["nested"] = [{"operation_id": "op.1"}, {"operation_id": "op.2"}]
    events[2]["condition_severity"] = "ERROR"
    original = deepcopy(events)
    bundle = forensic_bundle(
        {
            "events": events[::-1],
            "truncated": True,
            "next_before_sequence": 1,
            "retention": {"pruned": True},
        },
        program_run_id="run.1",
        exported_at_ns=base,
    )
    assert events == original
    assert bundle["latency"]["acceptance_to_execution"]["p99"] == "3"
    assert bundle["latency"]["acceptance_to_terminal"]["p99"] == "11"
    assert bundle["execution_digest"]["incomplete_operation_count"] == 1
    assert bundle["execution_digest"]["operations"][0]["latest_state"]["state"] == "completed"
    incident = bundle["incidents"][0]
    assert incident["causal_references"]["operation_id"] == ["op.1", "op.2"]
    assert incident["causal_references"]["scene_id"] == ["scene.at.incident"]
    assert bundle["blackboard"]["samples"][0]["values"]["grasp_confirmed"]["value"] is True
    trace = bundle["normalized_trace"]
    assert trace["schema_version"] == "manyforge.ros2_medkit_trace.v2"
    assert trace["parent_run_ids"] == ["parent.1"]
    assert (
        trace["records"][2]["operation_id"] is None
    )  # ambiguity is not a fabricated unique identity
    assert trace["coverage"]["truncated"] is True
    assert trace["records"][0]["monotonic_ns"] == str(base)


def test_same_revision_source_order_survives_late_feedback_and_later_terminal_ack():
    accepted = record(1, state="queued", kind="operation_accepted", time_ns=100)
    executing = record(4, time_ns=103)  # arrived after terminal/acknowledgement
    executing["event"]["payload"]["native_sequence"] = 2
    completed = record(2, state="completed", time_ns=111)
    completed["event"]["payload"]["native_sequence"] = 3
    ack = record(
        3,
        state="completed",
        kind="operation_acknowledge_completed_accepted",
        time_ns=10_000,
    )
    ack["event"]["payload"]["native_sequence"] = 4
    page = {
        "events": [accepted, executing, completed, ack],
        "truncated": False,
        "next_before_sequence": None,
    }
    bundle = forensic_bundle(page, program_run_id="run.1", exported_at_ns=10_001)
    operation = bundle["execution_digest"]["operations"][0]
    assert operation["latest_state"]["state"] == "completed"
    assert operation["latest_native_sequence"] == 4
    assert operation["latest_revision"] == 1
    assert bundle["latency"]["acceptance_to_execution"]["p99"] == "3"
    assert bundle["latency"]["acceptance_to_terminal"]["p99"] == "11"


# A retained v1 original keeps its recorded version and attribution rule.
@pytest.mark.parametrize(
    "schema_version",
    [
        "manyforge.program_diagnostic.v1",
        "manyforge.program_diagnostic.v2",
        "manyforge.program_diagnostic.v3",
        "manyforge.program_diagnostic.v4",
    ],
)
def test_program_values_export_keeps_partial_capture_and_delivery_gaps_explicit(
    schema_version,
):
    event = record(1)
    event["event"]["payload"] = {
        "schema_version": schema_version,
        "program_run_id": "run.1",
        "blackboard": {"ready": False, "program_run_id": "foreign.run"},
        "omitted_blackboard_count": 8,
        "delivery_loss_count": 3,
    }
    if schema_version in {"manyforge.program_diagnostic.v1", "manyforge.program_diagnostic.v2"}:
        # Retained v1/v2 originals still carry withdrawn runtime-override values;
        # they are read as recorded but never become identities or samples.
        event["event"]["payload"].update(
            {
                "runtime_overrides": {"program_run_id": "override.run"},
                "omitted_override_count": 1,
            }
        )
    bundle = forensic_bundle(
        {"events": [event], "truncated": False, "next_before_sequence": None},
        program_run_id="run.1",
        exported_at_ns=101,
    )
    sample = bundle["blackboard"]["samples"][0]
    assert sample["values"] == {"ready": False, "program_run_id": "foreign.run"}
    assert sample["causal_references"]["program_run_id"] == ["run.1"]
    assert bundle["normalized_trace"]["records"][0]["program_run_id"] == "run.1"
    assert "runtime_overrides" not in sample
    assert (sample["omitted_value_count"], sample["delivery_loss_count"]) == (8, 3)


@pytest.mark.parametrize("late_order", [None, str(2**53 + 1)])
def test_private_capture_order_prevents_delayed_storage_rebound(late_order):
    current = record(1, state="stopped", time_ns=100)
    current["event"]["payload"]["journal_payload"]["operation_state"]["journal_capture_order"] = (
        str(2**63 - 1)
    )
    delayed = record(2, state="stopping", time_ns=100)
    if late_order is not None:
        delayed["event"]["payload"]["journal_payload"]["operation_state"][
            "journal_capture_order"
        ] = late_order
    bundle = forensic_bundle(
        {
            "events": [current, delayed],
            "truncated": False,
            "next_before_sequence": None,
        },
        program_run_id="run.1",
        exported_at_ns=101,
    )
    operation = bundle["execution_digest"]["operations"][0]
    assert operation["latest_state"]["state"] == "stopped"
    assert operation["latest_native_sequence"] == 1
    assert len(bundle["normalized_trace"]["records"]) == 2


def test_latency_uses_first_captured_terminal_not_first_stored_terminal():
    accepted = record(1, kind="operation_accepted", state="queued", time_ns=100)
    acknowledgement = record(
        2,
        kind="operation_acknowledge_completed_accepted",
        state="completed",
        time_ns=10_000,
    )
    completed = record(3, state="completed", time_ns=110)
    for event, order in ((accepted, 1), (acknowledgement, 3), (completed, 2)):
        event["event"]["payload"]["journal_payload"]["operation_state"]["journal_capture_order"] = (
            str(order)
        )
    bundle = forensic_bundle(
        {
            "events": [accepted, acknowledgement, completed],
            "truncated": False,
            "next_before_sequence": None,
        },
        program_run_id="run.1",
        exported_at_ns=10_001,
    )
    assert bundle["execution_digest"]["operations"][0]["latest_native_sequence"] == 2
    assert bundle["latency"]["acceptance_to_terminal"]["p99"] == "10"


@pytest.mark.parametrize(
    "payload", [None, "historical text", 12, [1, {"operation_id": "op.array"}]]
)
def test_export_preserves_every_valid_historical_json_root(payload):
    event = record(1)
    event["event"]["payload"] = payload
    bundle = forensic_bundle(
        {"events": [event], "truncated": False, "next_before_sequence": None},
        program_run_id=None,
        exported_at_ns=1,
    )
    assert bundle["normalized_trace"]["records"][0]["attributes"] == payload
    assert not bundle["execution_digest"]["operations"]


def test_missing_native_order_is_not_inferred_from_ingest_sequence_or_revision():
    event = record(1, state="completed")
    del event["event"]["payload"]["native_sequence"]
    bundle = forensic_bundle(
        {"events": [event], "truncated": False, "next_before_sequence": None},
        program_run_id="run.1",
        exported_at_ns=1,
    )
    operation = bundle["execution_digest"]["operations"][0]
    assert operation["latest_state"] is None
    assert operation["source_order_unknown_count"] == 1
    assert bundle["latency"]["acceptance_to_terminal"]["sample_count"] == 0


def test_clipped_reference_scan_never_claims_a_unique_identity():
    event = record(1)
    event["event"]["payload"] = [
        {"operation_id": "op.first"},
        *([0] * 8192),
        {"operation_id": "op.last"},
    ]
    bundle = forensic_bundle(
        {"events": [event], "truncated": False, "next_before_sequence": None},
        program_run_id=None,
        exported_at_ns=1,
    )
    row = bundle["normalized_trace"]["records"][0]
    assert row["references_truncated"] is True
    assert row["causal_references"]["operation_id"] == ["op.first"]
    assert row["operation_id"] is None


def test_unknown_clock_missing_blackboard_and_empty_latency_stay_explicit():
    bundle = forensic_bundle(
        {
            "events": [record(1, clock=None)],
            "truncated": False,
            "next_before_sequence": None,
        },
        program_run_id=None,
        exported_at_ns=10,
    )
    assert not bundle["blackboard"]["available"]
    assert bundle["latency"]["acceptance_to_terminal"]["sample_count"] == 0
    assert bundle["latency"]["acceptance_to_terminal"]["p99"] is None
    assert bundle["execution_digest"]["operations"] == []
    _, clipped = causal_references({"dense": [0] * 100_000})
    assert clipped
    with pytest.raises(ValueError, match="budget"):
        forensic_bundle({"events": [record(1)] * 257}, program_run_id=None, exported_at_ns=0)


def test_history_queries_pin_server_time_and_validate_identifiers_before_work():
    query = HistorianQueryRequest(
        recent_seconds=60, minimum_severity="ERROR", parent_node_attempt_id="parent.2"
    )
    native = query.native_arguments(now_utc_ns=100_000_000_000)
    assert (native["start_utc_ns"], native["end_utc_ns"]) == (
        40_000_000_000,
        100_000_000_000,
    )
    assert native["severities"] == ["ERROR", "CRITICAL"]
    assert native["correlations"] == {"parent_node_attempt_id": "parent.2"}
    assert HistorianDebugRequest(run_id="run.1").history_query().program_run_id == "run.1"
    for factory, values in (
        (HistorianQueryRequest, {"start_ns": 1}),
        (HistorianDebugRequest, {"run_id": "run.1", "program_run_id": "run.2"}),
        (HistorianDiscoveryRequest, {"kind": "component", "after": "e\u0301"}),
        (HistorianQueryRequest, {"minimum_severity": "ERROR", "severities": ["INFO"]}),
        (HistorianQueryRequest, {"search": "xy"}),
    ):
        with pytest.raises(ValidationError):
            factory(**values)


@pytest.mark.parametrize(
    "events,cursor,truncated",
    [
        ([], 1, True),
        ([record(2), record(2)], 2, True),
        ([record(2)], 3, True),
        ([record(2)], 2, False),
        ([record(True)], None, False),
    ],
)
def test_invalid_native_cursor_never_drives_a_debug_export_loop(events, cursor, truncated):
    with pytest.raises(HeadlessApiError):
        _history_page(
            {
                "schema_version": "page",
                "events": events,
                "next_before_sequence": cursor,
                "truncated": truncated,
            },
            "page",
        )


# --- volatile motion trace debug-export member -------------------------------

from starlette.responses import JSONResponse

from manyforge_headless_api import service as service_module
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.service import (
    VOLATILE_MOTION_TRACE_MARGIN_BYTES,
    VOLATILE_MOTION_TRACE_MAX_BYTES,
    HeadlessService,
)

_MEMBER_OVERHEAD = len(',"volatile_motion_trace":') + VOLATILE_MOTION_TRACE_MARGIN_BYTES
_TRACE = {
    "schema_version": "manyforge.volatile_motion_trace.v1",
    "volatile": True,
    "coverage": {"truncated": False, "exported_bytes": 1},
    "goals": [{"goal_key": "g.1", "points": [{"sec": -1, "nanosec": 5}]}],
}


class _TraceExecution:
    def __init__(self, result):
        self.result = result
        self.allowances = []

    def read_volatile_motion_trace(self, allowance_bytes):
        self.allowances.append(allowance_bytes)
        if isinstance(self.result, Exception):
            raise self.result
        return deepcopy(self.result)


def _export_service(monkeypatch, execution):
    monkeypatch.setattr(service_module.time, "time_ns", lambda: 1_000)
    service = HeadlessService(object(), object(), execution=execution)
    service.history_export = lambda _query: {
        "schema_version": "manyforge.historian_export_page.v1",
        "events": [record(1)],
        "truncated": False,
        "next_before_sequence": None,
        "time_window": {"start_utc_ns": None, "end_utc_ns": None},
    }
    return service


def _body(content):
    return JSONResponse(content=content).body


def _baseline(monkeypatch):
    service = _export_service(monkeypatch, None)
    return _body(
        service.debug_export(HistorianQueryRequest(), include_volatile_motion_trace=False)
    )


def test_debug_export_includes_native_trace_unchanged_with_capped_allowance(monkeypatch):
    execution = _TraceExecution(_TRACE)
    bundle = _export_service(monkeypatch, execution).debug_export(HistorianQueryRequest())
    assert bundle["volatile_motion_trace"] == {"available": True, "trace": _TRACE}
    assert execution.allowances == [VOLATILE_MOTION_TRACE_MAX_BYTES]
    baseline = _baseline(monkeypatch)
    rendered = _body(bundle)
    # The member is appended after the unchanged historian bundle.
    assert rendered.startswith(baseline[:-1] + b',"volatile_motion_trace":')


def test_debug_export_allowance_is_exact_remaining_http_budget(monkeypatch):
    baseline = len(_baseline(monkeypatch))
    monkeypatch.setattr(service_module, "MAX_HTTP_RESPONSE_BYTES", baseline + 100_000)
    execution = _TraceExecution(_TRACE)
    _export_service(monkeypatch, execution).debug_export(HistorianQueryRequest())
    assert execution.allowances == [
        min(VOLATILE_MOTION_TRACE_MAX_BYTES, 100_000 - _MEMBER_OVERHEAD)
    ]


@pytest.mark.parametrize(
    "error,reason",
    [
        (HeadlessApiError("unknown_command", "unsupported"), "unknown_command"),
        (HeadlessApiError("execution_owner_unavailable", "timeout"), "execution_owner_unavailable"),
        (RuntimeError("boom"), "read_failed"),
    ],
)
def test_debug_export_survives_failed_trace_read_with_marker(monkeypatch, error, reason):
    bundle = _export_service(monkeypatch, _TraceExecution(error)).debug_export(
        HistorianQueryRequest()
    )
    assert bundle["volatile_motion_trace"] == {"available": False, "reason": reason}
    assert bundle["schema_version"] == "manyforge.debug_export.v1"
    without = _export_service(monkeypatch, None).debug_export(HistorianQueryRequest())
    assert without["volatile_motion_trace"] == {
        "available": False,
        "reason": "execution_unavailable",
    }


def test_debug_export_near_limit_uses_marker_or_leaves_baseline_identical(monkeypatch):
    baseline = _baseline(monkeypatch)
    marker = b',"volatile_motion_trace":{"available":false,"reason":"export_limit"}'
    monkeypatch.setattr(service_module, "MAX_HTTP_RESPONSE_BYTES", len(baseline) + 2_000)
    execution = _TraceExecution(_TRACE)
    bundle = _export_service(monkeypatch, execution).debug_export(HistorianQueryRequest())
    assert execution.allowances == []
    assert _body(bundle) == baseline[:-1] + marker + b"}"
    for room in (0, len(marker) - 1):
        monkeypatch.setattr(service_module, "MAX_HTTP_RESPONSE_BYTES", len(baseline) + room)
        bundle = _export_service(monkeypatch, execution).debug_export(HistorianQueryRequest())
        assert "volatile_motion_trace" not in bundle
        assert _body(bundle) == baseline
    assert execution.allowances == []


def test_debug_export_replaces_whole_inflated_trace_by_marker(monkeypatch):
    baseline = len(_baseline(monkeypatch))
    limit = baseline + 20_000
    monkeypatch.setattr(service_module, "MAX_HTTP_RESPONSE_BYTES", limit)
    inflated = {**_TRACE, "goals": [{"goal_key": "é" * 4_000}] * 4}
    execution = _TraceExecution(inflated)
    bundle = _export_service(monkeypatch, execution).debug_export(HistorianQueryRequest())
    assert execution.allowances == [20_000 - _MEMBER_OVERHEAD]
    assert bundle["volatile_motion_trace"] == {"available": False, "reason": "export_limit"}
    assert len(_body(bundle)) <= limit


def _trace_client(response):
    client = object.__new__(ExecutionCommandClient)
    client._attribution_transport = "internal"
    sent = []
    client._exchange = lambda command: sent.append(command) or response
    return client, sent


def test_execution_client_reads_volatile_trace_with_attributed_allowance():
    ok = {
        "api_version": "manyforge.execution_command_response.v4",
        "ok": True,
        "command": "read_volatile_motion_trace",
        "trace": _TRACE,
    }
    client, sent = _trace_client(ok)
    assert client.read_volatile_motion_trace(4096) is _TRACE
    assert sent == [
        {
            "api_version": "manyforge.execution_command.v3",
            "attribution": {"transport": "internal", "assurance": "unverified", "principal_id": None},
            "command": "read_volatile_motion_trace",
            "allowance_bytes": 4096,
        }
    ]
    for bad in (
        {**ok, "command": "status"},
        {**ok, "trace": []},
        {**ok, "trace": {**_TRACE, "schema_version": "manyforge.volatile_motion_trace.v2"}},
        {"api_version": ok["api_version"], "ok": False,
         "error": {"code": "unknown_command", "message": "unsupported"}},
    ):
        with pytest.raises(HeadlessApiError):
            _trace_client(bad)[0].read_volatile_motion_trace(4096)
    with pytest.raises(ValueError):
        client.read_volatile_motion_trace(0)


def _file_trace_client(tmp_path):
    trace = {**deepcopy(_TRACE), "request_owner_epoch": "owner.file"}
    payload = rfc8785.dumps(trace)
    digest = hashlib.sha256(payload).hexdigest()
    path = tmp_path / f"trace-{digest}.json"
    path.write_bytes(payload)
    path.chmod(0o600)
    metadata = {
        "schema_version": "manyforge.motion_trace_file.v1",
        "file": path.name,
        "bytes": len(payload),
        "sha256": f"sha256:{digest}",
        "request_owner_epoch": "owner.file",
        "status": "complete",
    }
    client, sent = _trace_client({
        "api_version": "manyforge.execution_command_response.v4",
        "ok": True,
        "command": "read_volatile_motion_trace",
        "trace_file": metadata,
    })
    return client, sent, trace, path


def test_debug_export_loads_opted_in_file_without_changing_historian(monkeypatch, tmp_path):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    client, sent, trace, _path = _file_trace_client(tmp_path)
    baseline = _baseline(monkeypatch)
    service = _export_service(monkeypatch, client)
    bundle = service.debug_export(HistorianQueryRequest())
    assert bundle["volatile_motion_trace"] == {"available": True, "trace": trace}
    assert _body(bundle).startswith(baseline[:-1] + b',"volatile_motion_trace":')
    assert len(sent) == 1
    assert sent[0]["allowance_bytes"] == VOLATILE_MOTION_TRACE_MAX_BYTES
    # The normalized-trace-only route still performs no trace/file request.
    without = service.debug_export(HistorianQueryRequest(), include_volatile_motion_trace=False)
    assert _body(without) == baseline
    assert len(sent) == 1


@pytest.mark.parametrize("condition,reason", [
    ("unconfigured", "motion_trace_file_invalid"),
    ("missing", "motion_trace_file_unavailable"),
    ("digest_changed", "motion_trace_file_invalid"),
])
def test_file_trace_failures_leave_export_available_with_explicit_reason(
    monkeypatch, tmp_path, condition, reason
):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    client, _sent, _trace, path = _file_trace_client(tmp_path)
    if condition == "unconfigured":
        monkeypatch.delenv("MANYFORGE_MOTION_TRACE_DIR")
    elif condition == "missing":
        path.unlink()
    else:
        payload = path.read_bytes()
        path.write_bytes(payload.replace(b"g.1", b"g.2", 1))
    bundle = _export_service(monkeypatch, client).debug_export(HistorianQueryRequest())
    assert bundle["schema_version"] == "manyforge.debug_export.v1"
    assert bundle["volatile_motion_trace"] == {"available": False, "reason": reason}
