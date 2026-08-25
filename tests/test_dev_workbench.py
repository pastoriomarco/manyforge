from __future__ import annotations

import importlib.util
from pathlib import Path
import socket
from types import SimpleNamespace
from typing import Any


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "manyforge_dev_workbench",
    REPOSITORY_ROOT / "scripts/dev-workbench.py",
)
assert SPEC is not None and SPEC.loader is not None
dev_workbench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dev_workbench)


def test_status_reads_the_backend_derived_scenario_without_reducing_it_again(
    monkeypatch: Any,
) -> None:
    response = {
        "schema_version": "manyforge.current_scenario.v1",
        "ready": True,
        "current_scenario": {"scenario_id": "lite6_pick_and_place"},
        "unavailable_reasons": [],
    }

    class _Client:
        def __init__(self, *_args: Any, **_kwargs: Any) -> None:
            pass

        def __enter__(self) -> "_Client":
            return self

        def __exit__(self, *_args: Any) -> None:
            pass

        def request(self, method: str, path: str) -> dict[str, Any]:
            assert (method, path) == ("GET", "/api/scenarios/current")
            return response

    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", _Client)

    status = dev_workbench.query_status(
        scenario_id="lite6_pick_and_place", composer_port=9020
    )

    assert status["current_scenario"] == response["current_scenario"]
    assert status["classification"] == {
        "kind": "development_workbench",
        "package_kind": "maintained_qualified",
        "physical_execution_authorized": False,
    }


def test_status_rejects_a_different_requested_scenario(monkeypatch: Any) -> None:
    class _Client:
        def __init__(self, *_args: Any, **_kwargs: Any) -> None:
            pass

        def __enter__(self) -> "_Client":
            return self

        def __exit__(self, *_args: Any) -> None:
            pass

        def request(self, _method: str, _path: str) -> dict[str, Any]:
            return {
                "schema_version": "manyforge.current_scenario.v1",
                "ready": True,
                "current_scenario": {"scenario_id": "other"},
                "unavailable_reasons": [],
            }

    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", _Client)

    status = dev_workbench.query_status(
        scenario_id="lite6_pick_and_place", composer_port=9020
    )

    assert status["ready"] is False
    assert status["current_scenario"] is None
    assert status["unavailable_reasons"] == ["requested_scenario_mismatch"]


def test_status_command_reads_the_current_scenario_without_package_mutation(
    monkeypatch: Any,
    capsys: Any,
) -> None:
    observed: list[str | None] = []

    def query_status(
        *, scenario_id: str | None, composer_port: int
    ) -> dict[str, Any]:
        assert composer_port == 9020
        observed.append(scenario_id)
        return {
            "schema_version": "manyforge.current_scenario.v1",
            "ready": True,
            "current_scenario": {"scenario_id": "another-native-scenario"},
            "unavailable_reasons": [],
        }

    monkeypatch.setattr(dev_workbench, "query_status", query_status)

    def unexpected_import() -> None:
        raise AssertionError("status mutated package cache")

    monkeypatch.setattr(dev_workbench, "_import_package", unexpected_import)

    assert dev_workbench.main(["status"]) == 0

    assert observed == [None]
    assert "another-native-scenario" in capsys.readouterr().out


def test_existing_cache_snapshots_exact_native_closure_before_ui_start(
    monkeypatch: Any, tmp_path: Path
) -> None:
    scenario_root = tmp_path / "maintained-scenario-package"
    package_root = scenario_root / "retained-package"
    package_root.mkdir(parents=True)
    qualified_manager = scenario_root / "manyforge_manager_daemon"
    qualified_headless_root = scenario_root / "headless-python"
    built_manager = tmp_path / "build/manyforge_manager_daemon"
    built_extension = tmp_path / "build/manyforge_headless_py.fixture.so"
    native_host = scenario_root / "manyforge_native_execution_host"
    candidate_worker = scenario_root / "manyforge_roboplan_candidate_worker"
    built_manager.parent.mkdir(parents=True)
    for path, content in (
        (built_manager, b"qualified-manager"),
        (built_extension, b"qualified-headless-extension"),
        (native_host, b"native-host"),
        (candidate_worker, b"candidate-worker"),
    ):
        path.write_bytes(content)
        path.chmod(0o755)

    monkeypatch.setattr(dev_workbench, "SCENARIO_ROOT", scenario_root)
    monkeypatch.setattr(dev_workbench, "PACKAGE_ROOT", package_root)
    monkeypatch.setattr(
        dev_workbench, "QUALIFIED_MANAGER_BINARY", qualified_manager
    )
    monkeypatch.setattr(
        dev_workbench, "QUALIFIED_HEADLESS_ROOT", qualified_headless_root
    )
    monkeypatch.setattr(dev_workbench, "HEADLESS_BUILD", built_manager.parent)
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", built_manager)
    monkeypatch.setattr(dev_workbench, "NATIVE_EXECUTION_HOST", native_host)
    monkeypatch.setattr(
        dev_workbench, "NATIVE_CANDIDATE_WORKER", candidate_worker
    )

    dev_workbench._import_package()

    assert qualified_manager.read_bytes() == b"qualified-manager"
    assert (
        qualified_headless_root / built_extension.name
    ).read_bytes() == b"qualified-headless-extension"


def test_startup_requires_the_existing_manager_baseline_to_be_fully_converged() -> None:
    status = {
        "source": {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
        },
        "status": {
            "manager_ready": True,
            "runtime": {"converged": True},
        },
    }
    diff = {
        "source": {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
        },
        "converged": True,
    }

    assert dev_workbench._manager_projection_ready(status, diff) is True
    assert dev_workbench._manager_projection_ready(
        status,
        {**diff, "converged": False},
    ) is False
    assert dev_workbench._manager_projection_ready(
        {**status, "status": {**status["status"], "manager_ready": False}},
        diff,
    ) is False


def test_stale_socket_cleanup_removes_only_the_named_socket(tmp_path: Path) -> None:
    socket_path = tmp_path / "manager-command.sock"
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        listener.bind(str(socket_path))

        dev_workbench._remove_stale_socket(socket_path, "Manager command socket")

        assert not socket_path.exists()
    finally:
        listener.close()


def test_stale_socket_cleanup_rejects_non_socket_state(tmp_path: Path) -> None:
    state_path = tmp_path / "manager-command.sock"
    state_path.write_text("persistent state", encoding="utf-8")

    try:
        dev_workbench._remove_stale_socket(state_path, "Manager command socket")
    except dev_workbench.WorkbenchError as error:
        assert str(error) == f"Manager command socket is not a socket: {state_path}"
    else:
        raise AssertionError("non-socket state was removed")

    assert state_path.read_text(encoding="utf-8") == "persistent state"


def test_launcher_pins_the_image_and_separates_its_cache() -> None:
    launcher = (REPOSITORY_ROOT / "scripts/compose-dev.sh").read_text(
        encoding="utf-8"
    )

    assert "docker image inspect -f '{{.Id}}'" in launcher
    assert 'cache_root="$cache_base/images/$image_key"' in launcher
    assert '"$image_id" \\' in launcher


def test_cleanup_uses_bounded_reverse_owner_order(monkeypatch: Any) -> None:
    workbench = dev_workbench.Workbench(
        scenario_id="lite6_pick_and_place",
        headless_port=1,
        runner_port=2,
        composer_port=3,
        frontend_port=4,
    )
    workbench.processes = [
        ("Manager", SimpleNamespace()),
        ("Program runner", SimpleNamespace()),
        ("Headless API", SimpleNamespace()),
        ("Composer backend", SimpleNamespace()),
        ("Vite HMR", SimpleNamespace()),
    ]
    observed: list[str] = []
    monkeypatch.setattr(
        workbench,
        "_terminate",
        lambda process: observed.append(
            next(
                name
                for name, candidate in workbench.processes
                if candidate is process
            )
        ),
    )
    monkeypatch.setattr(
        workbench, "_shutdown_manager", lambda: observed.append("Manager")
    )

    workbench.close()

    assert observed == [
        "Vite HMR",
        "Composer backend",
        "Program runner",
        "Manager",
        "Headless API",
    ]
