"""Regression tests for qualification launcher process cleanup."""
from __future__ import annotations

import importlib.util
from pathlib import Path
from typing import Any

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]


def _load_run_scenarios_module() -> Any:
    module_path = _REPO_ROOT / "scripts" / "run-scenarios.py"
    spec = importlib.util.spec_from_file_location("manyforge_run_scenarios_test", module_path)
    assert spec is not None
    assert spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class _NeverHealthyClient:
    def __init__(self, *_args: Any, **_kwargs: Any) -> None:
        pass

    def __enter__(self) -> "_NeverHealthyClient":
        return self

    def __exit__(self, *_args: Any) -> None:
        return None

    def get(self, _path: str) -> None:
        raise RuntimeError("not ready")


class _FakeProcess:
    def __init__(self, *_args: Any, **_kwargs: Any) -> None:
        self.terminated = False
        self.killed = False
        self.wait_timeouts: list[float] = []
        self.returncode: int | None = None

    def poll(self) -> int | None:
        return self.returncode

    def terminate(self) -> None:
        self.terminated = True

    def wait(self, timeout: float | None = None) -> int:
        self.wait_timeouts.append(float(timeout or 0.0))
        self.returncode = -15 if self.terminated else -9
        return self.returncode

    def kill(self) -> None:
        self.killed = True
        self.returncode = -9


class _FakeMonotonic:
    def __init__(self) -> None:
        self.now = 0.0

    def __call__(self) -> float:
        self.now += 0.01
        return self.now


def test_local_process_launcher_cleans_up_on_forced_health_timeout(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    run_scenarios = _load_run_scenarios_module()
    fake_process = _FakeProcess()

    monkeypatch.setattr(
        run_scenarios.subprocess,
        "Popen",
        lambda *_args, **_kwargs: fake_process,
    )
    monkeypatch.setattr(run_scenarios.httpx, "Client", _NeverHealthyClient)
    monkeypatch.setattr(run_scenarios.time, "monotonic", _FakeMonotonic())
    monkeypatch.setattr(run_scenarios.time, "sleep", lambda _seconds: None)

    launcher = run_scenarios.ManagedComposerProcess(
        workspace_root=_REPO_ROOT,
        catalog_path=_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml",
        host="127.0.0.1",
        port=19000,
        log_level="INFO",
        kernel_mode="mock",
        mock_execute_duration_s=0.0,
        output_dir=tmp_path,
        wait_timeout_s=0.02,
    )

    with pytest.raises(RuntimeError, match="Timed out waiting for Composer health endpoint"):
        launcher.start()

    assert fake_process.terminated is True
    assert fake_process.killed is False
    assert fake_process.wait_timeouts == [10.0]
    assert launcher._stdout_handle is not None
    assert launcher._stderr_handle is not None
    assert launcher._stdout_handle.closed
    assert launcher._stderr_handle.closed


class _AlwaysHealthyClient:
    """Answers /api/health like a composer that was already listening."""

    def __init__(self, *_args: Any, **_kwargs: Any) -> None:
        pass

    def __enter__(self) -> "_AlwaysHealthyClient":
        return self

    def __exit__(self, *_args: Any) -> None:
        return None

    def get(self, _path: str) -> Any:
        class _Response:
            status_code = 200

        return _Response()


def _make_launcher(run_scenarios: Any, tmp_path: Path) -> Any:
    return run_scenarios.ManagedComposerProcess(
        workspace_root=_REPO_ROOT,
        catalog_path=_REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml",
        host="127.0.0.1",
        port=19000,
        log_level="INFO",
        kernel_mode="mock",
        mock_execute_duration_s=0.0,
        output_dir=tmp_path,
        wait_timeout_s=0.02,
    )


def test_local_process_launcher_refuses_port_already_serving_a_composer(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """The launcher must never adopt a composer this harness did not spawn."""
    run_scenarios = _load_run_scenarios_module()
    spawned: list[Any] = []

    def _forbidden_popen(*_args: Any, **_kwargs: Any) -> Any:
        spawned.append(True)
        return _FakeProcess()

    monkeypatch.setattr(run_scenarios.subprocess, "Popen", _forbidden_popen)
    monkeypatch.setattr(run_scenarios.httpx, "Client", _AlwaysHealthyClient)

    launcher = _make_launcher(run_scenarios, tmp_path)

    with pytest.raises(RuntimeError, match="Refusing to launch"):
        launcher.start()

    assert spawned == []


def test_local_process_launcher_rejects_foreign_readiness_after_child_death(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """A readiness 200 racing a bind-failed child must not become adoption.

    The pre-spawn probe sees a free port (the foreign composer comes up in
    the race window); the child dies during the post-readiness grace while
    the foreign composer keeps answering 200.
    """
    run_scenarios = _load_run_scenarios_module()
    fake_process = _FakeProcess()
    probe_calls: list[bool] = []

    class _ForeignAfterSpawnClient:
        def __init__(self, *_args: Any, **_kwargs: Any) -> None:
            pass

        def __enter__(self) -> "_ForeignAfterSpawnClient":
            return self

        def __exit__(self, *_args: Any) -> None:
            return None

        def get(self, _path: str) -> Any:
            if not probe_calls:
                probe_calls.append(True)
                raise RuntimeError("pre-spawn probe sees a free port")

            class _Response:
                status_code = 200

            return _Response()

    def _dies_during_grace(_seconds: float) -> None:
        if _seconds == run_scenarios.ManagedComposerProcess._POST_READY_GRACE_S:
            fake_process.returncode = 1

    monkeypatch.setattr(
        run_scenarios.subprocess,
        "Popen",
        lambda *_args, **_kwargs: fake_process,
    )
    monkeypatch.setattr(run_scenarios.httpx, "Client", _ForeignAfterSpawnClient)
    monkeypatch.setattr(run_scenarios.time, "monotonic", _FakeMonotonic())
    monkeypatch.setattr(run_scenarios.time, "sleep", _dies_during_grace)

    launcher = _make_launcher(run_scenarios, tmp_path)

    with pytest.raises(RuntimeError, match="foreign composer"):
        launcher.start()

    assert launcher._stdout_handle is not None
    assert launcher._stdout_handle.closed
