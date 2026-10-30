"""Program Runner HTTP control boundaries preserve release ordering."""

from types import SimpleNamespace

import pytest
from fastapi.testclient import TestClient

from manyforge_program_runner.app import create_app

from .test_failure_release_revision import _RUN, _publish, harness


@pytest.mark.parametrize(
    ("route", "method"),
    [
        ("motion-start", "start_motion"),
        ("motion-resume", "resume_motion"),
        ("resume-full-stop", "resume_full_stop"),
    ],
)
def test_motion_endpoint_refreshes_tcp_before_release(
    harness, monkeypatch, route, method
) -> None:
    run = harness()
    if method == "resume_full_stop":
        _publish(run.owner, run.rows, "full_stopped")
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
    monkeypatch.setattr(run.runtime, "_verify_selected_motion_feedback", lambda *_args: None)
    monkeypatch.setattr(run.host, method, release)
    request = {
        "request_id": "control.request",
        "request_owner_epoch": "client.1",
        "source_label": "test",
        "program_run_id": _RUN,
        "expected_owner_epoch": run.runtime.owner_epoch,
        "expected_run_revision": run.owner.read(_RUN).run_revision,
        "reason": "operator",
    }
    if method != "resume_full_stop":
        request.update(expected_selection_revision=0, group_ids=["robot_group"])
    with TestClient(create_app(run.runtime)) as client:
        accepted = client.post(f"/api/program/runs/{_RUN}/{route}", json=request)
        assert accepted.status_code == 200
        assert calls == ["tcp", "release"]


@pytest.mark.parametrize("action", ["continue_with_failure", "retry_runtime_step"])
def test_recovery_endpoint_preserves_release_order(
    harness, monkeypatch, action
) -> None:
    run = harness()
    run.offer["actions"] = [{"action_id": action, "enabled": True}]
    calls = []
    original_release = getattr(
        run.owner,
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

    monkeypatch.setattr(run.owner, "continue_failure_pause", release)
    monkeypatch.setattr(run.owner, "retry_failure_pause", release)
    monkeypatch.setattr(run.runtime, "_refresh_active_tcp", tcp)
    monkeypatch.setattr(run.host, "resume_recovery", resume)
    request = {
        "request_id": "control.request",
        "request_owner_epoch": "client.1",
        "source_label": "test",
        "expected_owner_epoch": run.runtime.owner_epoch,
        "offer_id": run.offer["offer_id"],
        "offer_revision": run.offer["offer_revision"],
        "action_id": action,
    }
    with TestClient(create_app(run.runtime)) as client:
        accepted = client.post("/api/cycle/recovery-actions", json=request)
        assert accepted.status_code == 200
        assert accepted.json()["accepted"] is True
        assert calls == ["release", "tcp", "resume"]
        assert run.effects == ["attempt.1"]
