"""Production Program-runner admission, replay, and lifecycle glue."""

from __future__ import annotations

from copy import deepcopy
from types import SimpleNamespace
from typing import Any

import pytest

from manyforge_headless_api.models import (
    ProgramRunStartRequest,
    ProgramRunStopRequest,
)
from manyforge_program_runner.runtime import (
    ProgramRunnerConfig,
    ProgramRunnerError,
    ProgramRuntime,
)


_DIGEST_A = "sha256:" + "a" * 64
_DIGEST_B = "sha256:" + "b" * 64
_ACTIVE_STATES = {"running", "intervention_required", "stopping", "finalizing"}


class _Headless:
    """Construction-complete transport whose unused effects fail loudly."""

    _PORT_METHODS = {
        "acknowledge_completed_operation",
        "commit_scene_transaction",
        "cclc_component",
        "cclc_route_templates",
        "component_lifecycle_status",
        "commit_program_model_attempt",
        "execute",
        "execution_status",
        "manager_graph",
        "operation",
        "operation_runtime_scale",
        "pause_operation",
        "reset_operation",
        "resume_operation",
        "scene",
        "set_operation_runtime_scale",
        "stop_operations",
        "submit_component_lifecycle",
        "submit_route_selection",
        "route_selection_status",
    }

    def __init__(self) -> None:
        self.applied_reads = 0
        self.selection_reads = 0

    def program_applied(self) -> dict[str, Any]:
        self.applied_reads += 1
        return {
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": _DIGEST_A,
            "source": {"record_digest": _DIGEST_B},
        }

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
        self.selection_reads += 1
        assert scenario_id == "cell-pick"
        return {"scenario_id": scenario_id}

    def __getattr__(self, name: str):
        if name not in self._PORT_METHODS:
            raise AttributeError(name)

        def unexpected(*_args: object, **_kwargs: object) -> None:
            raise AssertionError(f"unexpected effectful headless call: {name}")

        return unexpected


class _Snapshot:
    def __init__(
        self,
        program_run_id: str,
        *,
        owner_id: str,
        owner_epoch: str,
        state: str,
        revision: int,
    ) -> None:
        self.capture = SimpleNamespace(program_run_id=program_run_id)
        self._value = {
            "schema_version": "manyforge.program_run_status.v1",
            "owner_id": owner_id,
            "owner_epoch": owner_epoch,
            "run_revision": revision,
            "state": state,
            "capture": {
                "schema_version": "manyforge.program_run_capture.v1",
                "program_run_id": program_run_id,
            },
            "tree_status": "running",
            "active_operation_count": 1 if state in _ACTIVE_STATES else 0,
            "reason_code": None,
            "message": "",
        }

    @property
    def state(self) -> str:
        return self._value["state"]

    @property
    def owner_epoch(self) -> str:
        return self._value["owner_epoch"]

    def to_dict(self) -> dict[str, Any]:
        return deepcopy(self._value)


class _Owner:
    def __init__(self) -> None:
        self.rows: dict[str, _Snapshot] = {}
        self.reads = 0

    def list_snapshots(
        self,
        *,
        active_only: bool,
        limit: int,
    ) -> tuple[tuple[_Snapshot, ...], bool]:
        rows = tuple(
            row
            for _identity, row in sorted(self.rows.items())
            if not active_only or row.state in _ACTIVE_STATES
        )
        return rows[:limit], len(rows) > limit

    def read(self, program_run_id: str) -> _Snapshot:
        self.reads += 1
        return self.rows[program_run_id]


class _Host:
    def __init__(self, owner: _Owner, *, owner_id: str, owner_epoch: str) -> None:
        self.owner = owner
        self.owner_id = owner_id
        self.owner_epoch = owner_epoch
        self.started = 0
        self.start_admissions: list[object] = []
        self.stop_calls: list[tuple[str, str | None]] = []
        self.shutdowns = 0

    def start(self) -> None:
        self.started += 1

    def start_run(self, admission: object) -> _Snapshot:
        self.start_admissions.append(admission)
        program_run_id = f"program-run.{len(self.start_admissions)}"
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="running",
            revision=1,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def stop_run(
        self,
        program_run_id: str,
        *,
        reason: str | None,
    ) -> _Snapshot:
        self.stop_calls.append((program_run_id, reason))
        snapshot = _Snapshot(
            program_run_id,
            owner_id=self.owner_id,
            owner_epoch=self.owner_epoch,
            state="stopped",
            revision=2,
        )
        self.owner.rows[program_run_id] = snapshot
        return snapshot

    def shutdown(self) -> None:
        self.shutdowns += 1


class _SimulationControl:
    def __init__(self) -> None:
        self.closed = 0

    def bind(self, _context: object) -> None:
        pass

    def unbind(self, _program_run_id: str) -> None:
        pass

    def start(self, *_args: object, **_kwargs: object) -> None:
        raise AssertionError("unexpected simulation task")

    def observe(self, _task_id: str) -> None:
        raise AssertionError("unexpected simulation task")

    def cancel(self, *_args: object) -> None:
        raise AssertionError("unexpected simulation task")

    def acknowledge(self, *_args: object) -> None:
        raise AssertionError("unexpected simulation task")

    def close(self) -> None:
        self.closed += 1


class _ModelTransport:
    def __init__(self) -> None:
        self.closed = 0

    def validate_endpoint(self, **_kwargs: object) -> None:
        pass

    def complete(self, *_args: object, **_kwargs: object) -> None:
        raise AssertionError("unexpected model request")

    def close(self) -> None:
        self.closed += 1


class _SnapshotSource:
    def __init__(self) -> None:
        self.closed = 0

    def capture_snapshot(self, **_kwargs: object) -> None:
        raise AssertionError("unexpected snapshot request")

    def close(self) -> None:
        self.closed += 1


def _start_request(*, request_id: str = "request.start.1") -> ProgramRunStartRequest:
    return ProgramRunStartRequest.model_validate(
        {
            "request_id": request_id,
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": _DIGEST_A,
            "expected_applied_record_digest": _DIGEST_B,
        }
    )


def test_runtime_replays_exact_requests_and_keeps_one_active_product_run(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    headless = _Headless()
    admitted = object()
    joins: list[tuple[dict[str, Any], dict[str, Any]]] = []

    def join(applied: dict[str, Any], live: dict[str, Any], **_kwargs: object):
        joins.append((deepcopy(applied), deepcopy(live)))
        return admitted

    monkeypatch.setattr(
        "manyforge_program_runner.runtime.build_applied_program_run_admission",
        join,
    )
    runtime = ProgramRuntime(
        ProgramRunnerConfig(
            headless_url="http://headless.test",
            owner_id="program-runner.cell-a",
            owner_epoch="program-runner.epoch.1",
        ),
        headless_client=headless,  # type: ignore[arg-type]
    )
    runtime._host.shutdown()
    owner = _Owner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
    )
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]

    runtime.start()
    request = _start_request()
    started = runtime.start_run(request)
    replayed = runtime.start_run(request)

    assert started == replayed
    assert started["capture"]["program_run_id"] == "program-run.1"
    assert host.started == 1
    assert host.start_admissions == [admitted]
    assert headless.applied_reads == 1
    assert headless.selection_reads == 1
    assert len(joins) == 1

    with pytest.raises(ProgramRunnerError) as active:
        runtime.start_run(_start_request(request_id="request.start.2"))
    assert active.value.code == "program_run_already_active"
    assert headless.applied_reads == 1

    with pytest.raises(ProgramRunnerError) as conflict:
        runtime.start_run(
            ProgramRunStartRequest.model_validate(
                {
                    **request.model_dump(mode="json"),
                    "scenario_catalog_revision": "sha256:" + "c" * 64,
                }
            )
        )
    assert conflict.value.code == "program_run_request_conflict"

    stop = ProgramRunStopRequest.model_validate(
        {
            "request_id": "request.stop.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
            "program_run_id": "program-run.1",
            "expected_owner_epoch": runtime.owner_epoch,
            "reason": "operator stop",
        }
    )
    stopped = runtime.stop_run("program-run.1", stop)
    stop_replay = runtime.stop_run("program-run.1", stop)
    assert stopped == stop_replay
    assert host.stop_calls == [("program-run.1", "operator stop")]

    runtime.shutdown()
    assert host.shutdowns == 1
    assert host.stop_calls == [("program-run.1", "operator stop")]


def test_runtime_rejects_a_stale_applied_selection_before_tree_admission(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    headless = _Headless()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test"),
        headless_client=headless,  # type: ignore[arg-type]
    )
    runtime._host.shutdown()
    owner = _Owner()
    host = _Host(
        owner,
        owner_id=runtime.owner_id,
        owner_epoch=runtime.owner_epoch,
    )
    runtime._owner = owner  # type: ignore[assignment]
    runtime._host = host  # type: ignore[assignment]
    monkeypatch.setattr(
        headless,
        "program_applied",
        lambda: {
            "scenario_id": "another-scenario",
            "scenario_catalog_revision": _DIGEST_A,
            "source": {"record_digest": _DIGEST_B},
        },
    )

    runtime.start()
    with pytest.raises(ProgramRunnerError) as stale:
        runtime.start_run(_start_request())
    assert stale.value.code == "program_run_applied_selection_stale"
    assert host.start_admissions == []
    assert headless.selection_reads == 0
    runtime.shutdown()


def test_runtime_injects_and_closes_the_focused_simulation_owner() -> None:
    simulation = _SimulationControl()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test"),
        headless_client=_Headless(),  # type: ignore[arg-type]
        simulation_control=simulation,  # type: ignore[arg-type]
    )

    assert runtime._ports.simulator_control is simulation  # noqa: SLF001
    assert runtime._owner_availability[  # noqa: SLF001
        "simulator_control"
    ].available
    assert runtime.health()["simulation_control_available"] is True
    runtime.shutdown()
    runtime.shutdown()
    assert simulation.closed == 1


def test_runtime_injects_and_closes_the_program_snapshot_source() -> None:
    headless = _Headless()
    model = _ModelTransport()
    snapshot = _SnapshotSource()
    runtime = ProgramRuntime(
        ProgramRunnerConfig(headless_url="http://headless.test"),
        headless_client=headless,  # type: ignore[arg-type]
        model_transport=model,  # type: ignore[arg-type]
        snapshot_source=snapshot,  # type: ignore[arg-type]
    )

    assert runtime._assistant is not None  # noqa: SLF001
    assert runtime.health()["assistant_snapshot_source_available"] is True
    runtime.shutdown()
    runtime.shutdown()
    assert snapshot.closed == 1
    assert model.closed == 1
