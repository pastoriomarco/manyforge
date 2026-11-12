"""The Program runner never refuses a Stop.

It forwards every Stop to the owner and the execution Kernel, which applies
its own rules (an exact repeat is replayed, a changed one or an unknown run
is a Full Stop), or answers "no effect" for a run that ended in this
application start. Errors raised after the owner accepted a Stop reply with
the run's current state.
"""

from __future__ import annotations

from threading import Barrier, Thread
from types import SimpleNamespace
from typing import Any

import pytest

from manyforge_behavior.program_run_owner import ProgramRunOwnerError
from manyforge_behavior.program_scheduling import ProgramSchedulingError
from manyforge_headless_api.models import (
    ProgramRunStopRequest,
    ProgramSelectionStopRequest,
)
from manyforge_program_runner.cycle_journal import ProgramCycleJournal
from manyforge_program_runner.runtime import (
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
)
from manyforge_program_runner.tests.test_runtime import (
    _Admission,
    _bypass_reset_scene_preparation,
    _bypass_scene_preparation,
    _Headless,
    _Host,
    _Owner,
    _PreparedRunAdmission,
    _Snapshot,
    _start_request,
)


class _Scope:
    """The Kernel-facing scope port: records forwarded Program-run Stops."""

    def __init__(self) -> None:
        self.requests: list[tuple[str, str, str | None]] = []

    def request_stop(self, program_run_id: str, stop_request_id: str, reason: str | None):
        self.requests.append((program_run_id, stop_request_id, reason))
        return SimpleNamespace(program_run_id=program_run_id)


def _running(monkeypatch: pytest.MonkeyPatch, tmp_path) -> tuple[ProgramRuntime, _Host, _Scope]:
    headless = _Headless()
    admitted = _PreparedRunAdmission()
    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        lambda _applied, **_kwargs: admitted,
    )
    _bypass_scene_preparation(monkeypatch)
    _bypass_reset_scene_preparation(monkeypatch)
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_id="program-runner.cell-a",
            owner_epoch="program-runner.epoch.1",
        ),
        headless_client=headless,  # type: ignore[arg-type]
        execution_transport=headless,  # type: ignore[arg-type]
        admission_client=_Admission(),  # type: ignore[arg-type]
        cycle_journal=ProgramCycleJournal(
            tmp_path / "cycle.sqlite3",
            owner_epoch="program-runner.epoch.1",
        ),
    )
    runtime._host.shutdown()  # noqa: SLF001
    owner = _Owner()
    host = _Host(owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch)
    host.read = lambda program_run_id: owner.rows[program_run_id]  # type: ignore[attr-defined]
    runtime._owner = owner  # type: ignore[assignment]  # noqa: SLF001
    runtime._host = host  # type: ignore[assignment]  # noqa: SLF001
    scope = _Scope()
    runtime._native = SimpleNamespace(scope=scope)  # type: ignore[assignment]  # noqa: SLF001
    runtime.start()
    started = runtime.start_run(_start_request())
    assert started["capture"]["program_run_id"] == "program-cycle.1"
    return runtime, host, scope


def _stop(run_id: str = "program-cycle.1", request_id: str = "request.stop.1", **changes: Any):
    return ProgramRunStopRequest.model_validate({
        "request_id": request_id,
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
        "program_run_id": run_id,
        "expected_owner_epoch": "program-runner.epoch.1",
        "reason": "operator Stop",
        **changes,
    })


def _motion_stop(run_id: str = "program-cycle.1", request_id: str = "request.motion.1",
                 **changes: Any):
    return ProgramSelectionStopRequest.model_validate({
        "request_id": request_id,
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
        "program_run_id": run_id,
        "expected_owner_epoch": "program-runner.epoch.1",
        "reason": "operator motion Stop",
        **changes,
    })


@pytest.mark.parametrize("kind", ["motion", "full"])
def test_a_concurrent_same_id_stop_reaches_the_owner_each_time(
    monkeypatch: pytest.MonkeyPatch, tmp_path, kind: str
) -> None:
    """No runner-side join: each copy reaches the owner and the Kernel with
    the same Stop id (the Kernel replays an exact repeat and promotes a
    changed one to a Full Stop with request_identity_conflict)."""
    runtime, host, _scope = _running(monkeypatch, tmp_path)
    calls = host.motion_stop_calls if kind == "motion" else host.full_stop_calls
    original = host.stop_motion if kind == "motion" else host.full_stop
    gate = Barrier(2)
    stop_ids: list[str | None] = []

    def both_inside(program_run_id: str, **kwargs: Any):
        stop_ids.append(kwargs.get("stop_request_id"))
        gate.wait(timeout=5)
        return original(program_run_id, **kwargs)

    if kind == "motion":
        host.stop_motion = both_inside  # type: ignore[method-assign]
        send = lambda request: runtime.stop_motion("program-cycle.1", request)  # noqa: E731
        first, changed = _motion_stop(), _motion_stop(reason="another reason")
    else:
        host.full_stop = both_inside  # type: ignore[method-assign]
        send = lambda request: runtime.full_stop("program-cycle.1", request)  # noqa: E731
        first, changed = _stop(), _stop(reason="another reason")
    replies: list[Any] = []
    threads = [Thread(target=lambda r=r: replies.append(send(r))) for r in (first, first)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=10)
    assert len(calls) == 2 and len(replies) == 2
    # The same id with different content also reaches the owner and Kernel.
    if kind == "motion":
        host.stop_motion = original  # type: ignore[method-assign]
    else:
        host.full_stop = original  # type: ignore[method-assign]
    send(changed)
    assert len(calls) == 3
    assert len(stop_ids) == 2 and len(set(stop_ids)) == 1


@pytest.mark.parametrize("kind", ["program", "motion", "full"])
def test_a_stop_for_a_run_that_ended_in_this_start_has_no_effect(
    monkeypatch: pytest.MonkeyPatch, tmp_path, kind: str
) -> None:
    runtime, host, scope = _running(monkeypatch, tmp_path)
    ended = _Snapshot(
        "program-cycle.0", owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch,
        state="succeeded", revision=7,
    )
    host.owner.rows["program-cycle.0"] = ended

    def unknown_to_owner(program_run_id: str, **_kwargs: Any):
        raise ProgramRunOwnerError("Program run does not exist")

    host.stop_motion = unknown_to_owner  # type: ignore[method-assign]
    host.full_stop = unknown_to_owner  # type: ignore[method-assign]
    if kind == "program":
        reply = runtime.stop_run("program-cycle.0", _stop("program-cycle.0"))
    elif kind == "motion":
        reply = runtime.stop_motion("program-cycle.0", _motion_stop("program-cycle.0"))
    else:
        reply = runtime.full_stop("program-cycle.0", _stop("program-cycle.0"))
    assert reply["state"] == "succeeded"
    assert reply["run_revision"] == 7
    assert scope.requests == []


@pytest.mark.parametrize("kind, code", [
    ("program", "program_run_not_found"),
    ("motion", "program_motion_stop_invalid"),
    ("full", "program_full_stop_invalid"),
])
def test_a_stop_for_a_never_seen_run_is_forwarded_to_the_kernel(
    monkeypatch: pytest.MonkeyPatch, tmp_path, kind: str, code: str
) -> None:
    """The Kernel applies its own rule: an unknown run is a Full Stop."""
    runtime, host, scope = _running(monkeypatch, tmp_path)

    def unknown_to_owner(program_run_id: str, **_kwargs: Any):
        raise ProgramRunOwnerError("Program run does not exist")

    host.stop_motion = unknown_to_owner  # type: ignore[method-assign]
    host.full_stop = unknown_to_owner  # type: ignore[method-assign]
    with pytest.raises(ProgramRunnerError) as forwarded:
        if kind == "program":
            runtime.stop_run("program-cycle.never", _stop("program-cycle.never"))
        elif kind == "motion":
            runtime.stop_motion("program-cycle.never", _motion_stop("program-cycle.never"))
        else:
            runtime.full_stop("program-cycle.never", _stop("program-cycle.never"))
    assert forwarded.value.code == code
    assert "forwarded to the execution Kernel" in str(forwarded.value)
    prefix = {"program": "execution", "motion": "motion", "full": "full"}[kind]
    request_id = "request.motion.1" if kind == "motion" else "request.stop.1"
    assert scope.requests == [(
        "program-cycle.never", f"{prefix}:composer.epoch.1:{request_id}",
        "operator motion Stop" if kind == "motion" else "operator Stop",
    )]


@pytest.mark.parametrize("kind", ["program", "motion", "full"])
def test_a_scheduling_failure_after_the_owner_accepted_answers_the_run(
    monkeypatch: pytest.MonkeyPatch, tmp_path, kind: str
) -> None:
    runtime, host, _scope = _running(monkeypatch, tmp_path)
    accepted = {"program": host.stop_run, "motion": host.stop_motion, "full": host.full_stop}[kind]

    def accept_then_fail_scheduling(program_run_id: str, **kwargs: Any):
        accepted(program_run_id, **kwargs)
        raise ProgramSchedulingError("scheduled Program-run set is full")

    if kind == "program":
        host.stop_run = accept_then_fail_scheduling  # type: ignore[method-assign]
        reply = runtime.stop_run("program-cycle.1", _stop())
        assert reply["state"] == "stopped"
        # Recorded as an accepted Stop, not as an unknown outcome.
        assert runtime.cycle_status()["state"] == "terminal_awaiting_reset"
    elif kind == "motion":
        host.stop_motion = accept_then_fail_scheduling  # type: ignore[method-assign]
        reply = runtime.stop_motion("program-cycle.1", _motion_stop())
        assert reply["state"] == "running"
    else:
        host.full_stop = accept_then_fail_scheduling  # type: ignore[method-assign]
        reply = runtime.full_stop("program-cycle.1", _stop())
        assert reply["state"] == "full_stopped"
    assert reply["capture"]["program_run_id"] == "program-cycle.1"
