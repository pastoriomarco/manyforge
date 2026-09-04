"""Focused contract tests for the O1.5-UI6 native qualification runner."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import time
from typing import Any

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[2]
_QUALIFICATION = _REPO_ROOT / "scripts" / "qualification"
sys.path.insert(0, str(_QUALIFICATION))
_SPEC = importlib.util.spec_from_file_location(
    "ui6_native_flow_test",
    _QUALIFICATION / "ui6_native_flow.py",
)
assert _SPEC is not None and _SPEC.loader is not None
ui6 = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = ui6
_SPEC.loader.exec_module(ui6)


def _stream_status(
    *,
    wakes: int,
    published: int,
    sent: int,
    credits: int = 0,
    clients: int = 2,
) -> dict[str, Any]:
    return {
        "schema_version": "manyforge.execution_observation_stream_status.v2",
        "source": {
            "connection_count": 1,
            "wake_count": wakes,
            "published_count": published,
            "error_count": 0,
        },
        "fanout": {
            "client_count": clients,
            "credit_count": credits,
            "sent_count": sent,
            "coalesced_frames_total": 0,
        },
    }


def test_client_baseline_waits_through_ambient_reconnect(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    statuses = iter(
        (
            _stream_status(wakes=0, published=0, sent=0, clients=0),
            _stream_status(wakes=0, published=0, sent=0, clients=1),
            _stream_status(wakes=0, published=0, sent=0, clients=1),
            _stream_status(wakes=0, published=0, sent=0, clients=1),
        )
    )
    timestamps = iter((0.0, 0.1, 0.2, 0.6, 1.0))
    monkeypatch.setattr(ui6, "_viewer_status", lambda _url: next(statuses))
    monkeypatch.setattr(ui6.time, "monotonic", lambda: next(timestamps))
    monkeypatch.setattr(ui6.time, "sleep", lambda _seconds: None)

    settled = ui6._wait_client_baseline(
        "http://composer.test",
        stable_s=0.75,
        timeout_s=5.0,
    )

    assert settled["fanout"]["client_count"] == 1


def test_counter_delta_keeps_renewal_separate_from_visual_work() -> None:
    delta = ui6._counter_delta(
        _stream_status(wakes=3, published=2, sent=4, credits=10),
        _stream_status(wakes=3, published=2, sent=4, credits=13),
    )

    assert delta == {
        "sourceConnections": 0,
        "sourceWakes": 0,
        "published": 0,
        "sourceErrors": 0,
        "credits": 3,
        "sent": 0,
        "coalesced": 0,
    }


def test_terminal_history_normalizes_the_existing_historian_envelope(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    payload = {
        "acceptance": {"accepted_at_ns": 1_000_000_000},
        "completed_at_ns": 1_050_000_000,
        "record_kind": "stop_terminal_projected",
        "stop_generation": 4,
        "stop_id": "stop.4",
    }
    page = {
        "events": [{
            "event": {
                "event_type": "stop_terminal_projected",
                "payload": {"journal_payload": payload},
            },
            "sequence": 12,
        }],
    }
    monkeypatch.setattr(ui6, "_http_json", lambda *_args, **_kwargs: page)

    terminal = ui6._wait_stop_terminal("http://composer.test", 11)

    assert terminal == {
        "event": page["events"][0]["event"],
        "payload": payload,
        "sequence": 12,
    }


def test_ordinary_stop_waits_for_one_owner_published_running_node(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    pages = iter(
        (
            {"runs": [{"state": "running", "node_statuses": {}}]},
            {
                "runs": [
                    {
                        "state": "running",
                        "node_statuses": {
                            "sequence": "running",
                            "move_to_rest": "running",
                        },
                        "capture": {"program_run_id": "run.1"},
                    }
                ]
            },
        )
    )
    sleeps: list[float] = []
    monkeypatch.setattr(ui6, "_http_json", lambda *_args, **_kwargs: next(pages))
    monkeypatch.setattr(ui6.time, "sleep", sleeps.append)

    active = ui6._wait_running_node("http://composer.test")

    assert active["capture"]["program_run_id"] == "run.1"
    assert sleeps == [ui6.POLL_SECONDS]


def test_stop_latency_uses_kernel_acceptance_and_settlement_timestamps() -> None:
    result = ui6._stop_latency(
        {
            "clickPerfMs": 100,
            "clickWallMs": 10_990,
            "dispatchPerfMs": 105,
            "dispatchWallMs": 10_995,
            "commandKind": "program_motion_stop",
            "receiptPerfMs": 130,
            "receiptWallMs": 11_020,
            "receipt": {
                "disposition": "received",
                "code": "program_motion_stop_received",
            },
            "controlIngressMonotonicNs": "995000000",
            "receiptEmittedMonotonicNs": "1010000000",
            "stoppingPerfMs": 120,
            "stoppedPerfMs": 180,
        },
        {
            "payload": {
                "acceptance": {"accepted_at_ns": 1_000_000_000},
                "completed_at_ns": 1_050_000_000,
                "stop_generation": 4,
                "stop_id": "stop.4",
            },
            "sequence": 12,
        },
        10_000_000_000,
    )

    assert result == {
        "clickToDispatchMs": 5,
        "dispatchToControlIngressMs": 0,
        "controlIngressToKernelAcceptanceMs": 5,
        "kernelAcceptanceToReceiptEmissionMs": 10,
        "receiptEmissionToBrowserMs": 10,
        "kernelAcceptanceToReceiptMs": 20,
        "kernelAcceptanceToStoppingPresentationMs": 10,
        "kernelAcceptanceToPhysicalSettlementMs": 50,
        "physicalSettlementToStoppedPresentationMs": 20,
        "clickToStoppedPresentationMs": 80,
        "transport": "websocket",
        "commandKind": "program_motion_stop",
        "receiptDisposition": "received",
        "receiptCode": "program_motion_stop_received",
        "stopId": "stop.4",
        "stopGeneration": 4,
        "historianSequence": 12,
    }


def test_slow_freeze_runs_off_the_collector_thread(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    entered = False

    def execute(_driver: object, _script: str) -> dict[str, float]:
        nonlocal entered
        entered = True
        time.sleep(0.05)
        return {"startedWallMs": 1, "endedWallMs": 51, "durationMs": 50}

    class Driver:
        timeout_s = 5.0

    monkeypatch.setattr(ui6, "_execute", execute)
    driver = Driver()

    worker, outcome, _started = ui6._start_slow_freeze(driver, 0.05)

    assert entered is True
    assert not worker.is_alive()
    assert outcome["browser"]["durationMs"] == 50
    assert driver.timeout_s == 30.05
