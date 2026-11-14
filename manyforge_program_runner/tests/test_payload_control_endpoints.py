"""Program Runner HTTP control boundaries preserve release ordering."""
import hashlib

from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient

from manyforge_program_runner.app import create_app

from .test_failure_release_revision import _RUN, _publish, harness
from .test_runtime import _join_command_threads


def _kb(*parts) -> bytes:
    """A deterministic 16-byte request key named by text, for readable tests."""

    return hashlib.md5("/".join(map(str, parts)).encode()).digest()


def _key(*parts) -> str:
    """The wire form (32 lowercase hex) of ``_kb``."""

    return _kb(*parts).hex()


@pytest.mark.parametrize(
    ("route", "method"),
    [
        ("motion-start", "start_motion"),
        ("motion-resume", "resume_motion"),
        ("resume-halt", "resume_halt"),
    ],
)
def test_motion_endpoint_refreshes_tcp_before_release(
    harness, monkeypatch, route, method
) -> None:
    run = harness()
    if method == "resume_halt":
        _publish(run.owner, run.rows, "halted")
    calls = []
    original_host = getattr(run.host, method)
    original_tcp = run.runtime._refresh_active_tcp  # noqa: SLF001

    def tcp():
        calls.append("tcp")
        original_tcp()

    def release(*args, **kwargs):
        calls.append("release")
        snapshot = original_host(*args, **kwargs)
        if method == "resume_motion":
            # The existing fake returns a string state; this host method's
            # production contract returns an enum-valued state.
            return SimpleNamespace(
                state=SimpleNamespace(value=snapshot.state),
                run_revision=snapshot.run_revision,
            )
        return snapshot

    monkeypatch.setattr(run.runtime, "_refresh_active_tcp", tcp)
    monkeypatch.setattr(run.runtime, "_require_run_readiness", lambda *_args: None)
    monkeypatch.setattr(run.host, method, release)
    request = {
        "request_key": _key("control.request"),
        "source_label": "test",
        "program_run_id": _RUN,
        "reason": "operator",
    }
    if method == "resume_halt":
        request.update(expected_run_revision=run.owner.read(_RUN).run_revision)
    else:
        request.update(expected_hold_revision=0, group_ids=["robot_group"])
    with TestClient(create_app(run.runtime)) as client:
        accepted = client.post(f"/api/program/runs/{_RUN}/{route}", json=request)
        assert accepted.status_code == 200
        # OD-214: answered at acceptance; the command runs on to its end.
        assert accepted.json()["command"]["accepted_at_ns"] is not None
        _join_command_threads()
        assert calls == ["tcp", "release"]
        assert run.runtime.cycle_status()["runner_commands"]["ended"][0]["end"][
            "outcome"] == "succeeded"


@pytest.mark.parametrize("action", ["continue_with_failure", "retry_runtime_step"])
def test_recovery_endpoint_preserves_release_order(
    harness, monkeypatch, action
) -> None:
    run = harness()
    run.offer["actions"] = [{"action_id": action, "enabled": True}]
    calls = []
    original_release = getattr(
        run.host,
        "continue_failure_pause" if action == "continue_with_failure" else "retry_failure_pause",
    )
    original_resume = run.host.resume_recovery
    original_tcp = run.runtime._refresh_active_tcp  # noqa: SLF001

    def release(*args, **kwargs):
        calls.append("release")
        return original_release(*args, **kwargs)

    def tcp():
        calls.append("tcp")
        original_tcp()

    def resume(*args, **kwargs):
        calls.append("resume")
        return original_resume(*args, **kwargs)

    monkeypatch.setattr(run.host, "continue_failure_pause", release)
    monkeypatch.setattr(run.host, "retry_failure_pause", release)
    monkeypatch.setattr(run.runtime, "_refresh_active_tcp", tcp)
    monkeypatch.setattr(run.host, "resume_recovery", resume)
    request = {
        "request_key": _key("control.request"),
        "source_label": "test",
        "offer_id": run.offer["offer_id"],
        "offer_revision": run.offer["offer_revision"],
        "action_id": action,
    }
    with TestClient(create_app(run.runtime)) as client:
        accepted = client.post("/api/cycle/recovery-actions", json=request)
        assert accepted.status_code == 200
        # OD-214: answered at acceptance; the command runs on to its end.
        assert accepted.json()["command"]["accepted_at_ns"] is not None
        _join_command_threads()
        assert calls == ["release", "tcp", "resume"]
        assert run.effects == [1]
        assert run.runtime.cycle_status()["runner_commands"]["ended"][0]["end"][
            "outcome"] == "succeeded"
