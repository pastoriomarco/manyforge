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
        hmi_port=None,
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
