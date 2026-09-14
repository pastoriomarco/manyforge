"""Qualification-reader guards, not substituted runtime acceptance evidence."""

import importlib.util
import importlib
from pathlib import Path
from types import SimpleNamespace

import pytest


@pytest.fixture
def read_terminal():
    path = Path(__file__).resolve().parents[1] / "ui6h_target_queries.py"
    spec = importlib.util.spec_from_file_location("ui6h_target_queries", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.read_stop_terminal


def terminal(stop_id="selected", **changes):
    return {
        "record_kind": "stop_terminal_projected",
        "payload": {
            "stop_id": stop_id,
            "owner_epoch": "owner",
            "details_truncated": False,
            **changes,
        },
    }


def test_exact_stop_can_follow_an_unrelated_stop_on_another_page(read_terminal):
    expected = terminal()
    calls = []

    def events(owner, cursor):
        calls.append((owner, cursor))
        return {
            "events": [terminal("other")] if cursor == 0 else [expected],
            "has_more": cursor == 0,
            "next_after_sequence": cursor + 16,
        }

    assert (
        read_terminal(SimpleNamespace(events=events), "owner", "selected")
        == expected["payload"]
    )
    assert calls == [("owner", 0), ("owner", 16)]


@pytest.mark.parametrize(
    "changes", [{"owner_epoch": "replaced"}, {"details_truncated": True}]
)
def test_crossed_or_truncated_terminal_is_not_qualification(read_terminal, changes):
    client = SimpleNamespace(events=lambda *_: {"events": [terminal(**changes)]})
    with pytest.raises(AssertionError):
        read_terminal(client, "owner", "selected")


def test_missing_terminal_remains_pending(read_terminal):
    client = SimpleNamespace(
        events=lambda *_: {"events": [terminal("other")], "has_more": False}
    )
    assert read_terminal(client, "owner", "selected") is None


def test_history_scan_is_bounded_and_requires_cursor_progress(read_terminal):
    for advance in (0, 16):
        calls = []

        def events(_owner, cursor):
            calls.append(cursor)
            return {
                "events": [],
                "has_more": True,
                "next_after_sequence": cursor + advance,
            }

        with pytest.raises(AssertionError):
            read_terminal(SimpleNamespace(events=events), "owner", "selected")
        assert len(calls) == (64 if advance else 1)


@pytest.mark.parametrize("read_unavailable", [False, True])
def test_stop_failure_captures_evidence_before_cleanup_without_hiding_error(
    monkeypatch, read_unavailable
):
    monkeypatch.syspath_prepend(str(Path(__file__).resolve().parents[1]))
    module = importlib.import_module("ui6_stop_latency")
    events = []
    driver = SimpleNamespace(
        session_id="browser",
        delete_session=lambda: events.append("browser_closed"),
    )
    original = module.CollectorError("Resume did not produce Running")

    def fail(*_args):
        raise original

    def read(_url, path):
        events.append(path)
        if read_unavailable:
            raise module.CollectorError("owner unavailable")
        return {"evidence": "current owner facts"}

    monkeypatch.setattr(module, "_wait_client_baseline", lambda *_: {})
    monkeypatch.setattr(module, "_client_count", lambda *_: 0)
    monkeypatch.setattr(module, "_create_browser", lambda *_, **__: driver)
    monkeypatch.setattr(module, "_wait_ready", fail)
    monkeypatch.setattr(module, "_http_json", read)
    monkeypatch.setattr(
        module, "_execute", lambda *_: events.append("expand_attention")
    )
    monkeypatch.setattr(module, "_dom_state", lambda *_: {"runtimeText": "reason"})
    monkeypatch.setattr(module, "_wait_status", lambda *_: None)
    with pytest.raises(module.CollectorError) as caught:
        module.collect_ui6_stop_latency(
            "primary", "pressure", "http://ui", "http://api"
        )
    assert caught.value is original
    assert len(original.__notes__) == 4
    assert events == [
        "/api/program/runs?active_only=true&limit=1",
        "/api/component-overview",
        "/api/execution/history?limit=16",
        "expand_attention",
        "browser_closed",
    ]
