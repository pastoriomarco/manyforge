from __future__ import annotations

import hashlib
import threading
from pathlib import Path
from typing import Any

import pytest
import rfc8785

from manyforge_headless_api.execution import (
    ExecutionCommandClient,
    ExecutionEventPump,
)
from manyforge_headless_api.native import HeadlessApiError


def _event(owner_epoch: str, sequence: int) -> dict[str, Any]:
    payload = {
        "api_version": "manyforge.operation_journal_record.v1",
        "effect_pending": False,
        "intent": {},
        "operation_state": {
            "operation_id": "operation.1",
            "operation_revision": 1,
            "participants": [],
            "program_binding_id": "binding.1",
            "program_run_id": "run.1",
            "state": "completed",
        },
        "owner_epoch": owner_epoch,
        "record_kind": "state_observed",
        "result": None,
        "runtime_scale_status": None,
    }
    canonical = rfc8785.dumps(payload)
    return {
        "effect_pending": False,
        "occurred_at_ns": 100 + sequence,
        "occurred_at_wall": f"2026-08-07T10:00:00.00000000{sequence}Z",
        "operation_id": "operation.1",
        "operation_revision": 1,
        "owner_epoch": owner_epoch,
        "payload": payload,
        "payload_digest": f"sha256:{hashlib.sha256(canonical).hexdigest()}",
        "record_kind": "state_observed",
        "request_id": None,
        "sequence": sequence,
    }


def _page(owner_epoch: str, events: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "events",
        "event_page": {
            "events": events,
            "has_more": False,
            "next_after_sequence": events[-1]["sequence"] if events else 0,
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v1",
        },
        "ok": True,
    }


def test_execution_client_validates_event_cursor_and_owner(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    client = ExecutionCommandClient(tmp_path)
    owner_epoch = "execution-host.epoch.1"
    response = _page(owner_epoch, [_event(owner_epoch, 1)])
    monkeypatch.setattr(client, "_exchange", lambda _command: response)
    page = client.events(owner_epoch, 0)
    assert page["next_after_sequence"] == 1

    response["event_page"]["next_after_sequence"] = 2
    with pytest.raises(HeadlessApiError) as invalid:
        client.events(owner_epoch, 0)
    assert invalid.value.code == "execution_command_response_invalid"

    response = _page(owner_epoch, [])
    response["event_page"]["has_more"] = True
    with pytest.raises(HeadlessApiError) as stalled:
        client.events(owner_epoch, 0)
    assert stalled.value.code == "execution_command_response_invalid"


class _RecordingCore:
    def __init__(self) -> None:
        self.ingested: list[dict[str, Any]] = []
        self.completed = threading.Event()
        self.unknown = threading.Event()

    def ingest_operation_event(self, **kwargs: Any) -> dict[str, Any]:
        self.ingested.append(kwargs)
        return {"status": "ingested"}

    def complete_operation_event_snapshot(self, **_kwargs: Any) -> dict[str, Any]:
        self.completed.set()
        return {"status": "complete"}

    def mark_operation_event_source_unknown(self, **_kwargs: Any) -> dict[str, Any]:
        self.unknown.set()
        return {"status": "marked_unknown"}


class _PagedExecution:
    def __init__(self) -> None:
        self.owner_epoch = "execution-host.epoch.pump"
        self.fail = False

    def status(self) -> dict[str, Any]:
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "command": "status",
            "ok": True,
            "status": {"owner_epoch": self.owner_epoch},
        }

    def events(
        self, owner_epoch: str, after_sequence: int, *, limit: int = 16
    ) -> dict[str, Any]:
        if self.fail:
            raise HeadlessApiError("execution_command_unavailable", "host lost")
        assert owner_epoch == self.owner_epoch
        assert limit == 1
        events = [] if after_sequence else [_event(owner_epoch, 1)]
        return {
            "events": events,
            "has_more": False,
            "next_after_sequence": after_sequence or 1,
            "owner_epoch": owner_epoch,
            "schema_version": "manyforge.execution_event_page.v1",
        }


def test_event_pump_delivers_without_frontend_polling_and_stops_cleanly() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    pump.shutdown()

    assert pump.last_error is None
    assert len(core.ingested) == 1
    assert core.ingested[0]["source_owner_epoch"] == execution.owner_epoch
    assert core.ingested[0]["source_sequence"] == 1
    assert core.ingested[0]["payload_json"] == rfc8785.dumps(
        _event(execution.owner_epoch, 1)["payload"]
    ).decode("utf-8")


def test_event_pump_marks_a_lost_current_source_unknown() -> None:
    core = _RecordingCore()
    execution = _PagedExecution()
    pump = ExecutionEventPump(  # type: ignore[arg-type]
        core, execution, poll_seconds=0.01, page_limit=1
    )
    pump.start()
    assert core.completed.wait(timeout=1.0)
    execution.fail = True
    assert core.unknown.wait(timeout=1.0)
    pump.shutdown()

    assert pump.last_error is not None
    assert "execution_command_unavailable" in pump.last_error
