from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import socket
from types import SimpleNamespace
from typing import Any

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "manyforge_dev_workbench",
    REPOSITORY_ROOT / "scripts/dev-workbench.py",
)
assert SPEC is not None and SPEC.loader is not None
dev_workbench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(dev_workbench)


def test_repository_bootstrap_uses_the_exact_source_owned_generator(
    tmp_path: Path, monkeypatch: Any
) -> None:
    generator = tmp_path / "manyforge_generate_canonical_empty_package"
    generator.write_text("generator", encoding="utf-8")
    generator.chmod(0o755)
    bootstrap = tmp_path / "bootstrap"
    observed: list[tuple[str, ...]] = []

    def run(command: tuple[str, ...], **_kwargs: Any) -> None:
        observed.append(command)
        selected_root = Path(command[2])
        selected_root.mkdir(parents=True, exist_ok=True)
        (selected_root / "canonical-empty-admission.c1_build_authority_cell.json").write_text("{}")

    monkeypatch.setattr(dev_workbench, "CANONICAL_EMPTY_GENERATOR", generator)
    monkeypatch.setattr(dev_workbench, "WORKSPACE", REPOSITORY_ROOT)
    monkeypatch.setattr(dev_workbench, "BOOTSTRAP_ROOT", bootstrap)
    monkeypatch.setattr(dev_workbench, "_run", run)

    root = dev_workbench._ensure_repository_bootstrap_package()
    assert root.parent == bootstrap
    assert observed == [
        (str(generator), "--output-root", str(root),
         "--cell-id", "c1_build_authority_cell", "--native-runtime-profile",
         str(dev_workbench.WORKSPACE / "manyforge_deployment/resources/maintained/catalog/"
             "runtime_profile/manyforge.native_execution_runtime.json")),
    ]
    assert dev_workbench._ensure_repository_bootstrap_package() == root
    generator.write_text("updated generator", encoding="utf-8")
    assert dev_workbench._ensure_repository_bootstrap_package() != root
    assert (root / "canonical-empty-admission.c1_build_authority_cell.json").exists()


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

        def health(self) -> dict[str, Any]:
            return {"status": "ok"}

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
        "package_kind": "repository_source",
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

        def health(self) -> dict[str, Any]:
            return {"status": "ok"}

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


def test_status_command_accepts_an_empty_authoring_workbench(
    monkeypatch: Any,
    capsys: Any,
) -> None:
    observed: list[str | None] = []

    def query_status(*, scenario_id: str | None, composer_port: int) -> dict[str, Any]:
        assert composer_port == 9020
        observed.append(scenario_id)
        return {
            "schema_version": "manyforge.current_scenario.v1",
            "ready": True,
            "current_scenario": {"scenario_id": "another-native-scenario"},
            "unavailable_reasons": [],
        }

    monkeypatch.setattr(dev_workbench, "query_status", query_status)

    assert dev_workbench.main(["status"]) == 0

    assert observed == [None]
    assert "another-native-scenario" in capsys.readouterr().out


@pytest.mark.parametrize("state", ["idle_after_reset", "terminal_awaiting_reset"])
def test_startup_accepts_contained_idle_and_resettable_terminal_cycles(
    state: str,
) -> None:
    client = SimpleNamespace(program_cycle_status=lambda: {"state": state})

    assert dev_workbench._startup_cycle_state(client) == state


def test_startup_rejects_an_uncontained_program_cycle() -> None:
    client = SimpleNamespace(program_cycle_status=lambda: {"state": "running"})

    with pytest.raises(
        dev_workbench.WorkbenchError,
        match="Program cycle is not contained at startup",
    ):
        dev_workbench._startup_cycle_state(client)


def test_backend_build_uses_current_source_and_only_production_targets(
    monkeypatch: Any, tmp_path: Path
) -> None:
    build = tmp_path / "headless"
    manager = build / "manyforge_manager_daemon"
    extension = build / "manyforge_headless_py.fixture.so"
    observed: list[tuple[str, ...]] = []

    def run(command: tuple[str, ...], **_kwargs: Any) -> None:
        observed.append(command)
        if command[0] == "cmake" and command[1] == "--build":
            manager.parent.mkdir(parents=True, exist_ok=True)
            manager.write_bytes(b"manager")
            manager.chmod(0o755)
            extension.write_bytes(b"headless")

    monkeypatch.setattr(dev_workbench, "HEADLESS_BUILD", build)
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", manager)
    monkeypatch.setattr(dev_workbench, "_run", run)
    monkeypatch.setattr(
        dev_workbench.subprocess,
        "run",
        lambda *_args, **_kwargs: SimpleNamespace(
            returncode=0, stdout="/pybind11/cmake\n"
        ),
    )
    monkeypatch.setenv("MANYFORGE_DEV_BUILD_JOBS", "3")

    assert dev_workbench._ensure_current_backend() == extension

    assert len(observed) == 2
    assert "-DBUILD_TESTING=OFF" in observed[0]
    assert observed[1][-5:] == (
        "manyforge_manager_daemon",
        "manyforge_headless_py",
        "--",
        "-k",
        "0",
    )
    assert "--parallel" in observed[1]
    assert observed[1][observed[1].index("--parallel") + 1] == "3"


def test_native_build_uses_current_source_and_only_runtime_targets(
    monkeypatch: Any, tmp_path: Path
) -> None:
    build = tmp_path / "roboplan"
    native_host = build / "manyforge_native_execution_host"
    candidate_worker = build / "manyforge_roboplan_candidate_worker"
    observed: list[tuple[str, ...]] = []

    def run(command: tuple[str, ...], **_kwargs: Any) -> None:
        observed.append(command)
        if command[0] == "cmake" and command[1] == "--build":
            build.mkdir(parents=True, exist_ok=True)
            for path in (native_host, candidate_worker):
                path.write_bytes(b"current-native")
                path.chmod(0o755)

    monkeypatch.setattr(dev_workbench, "ROBOPLAN_BUILD", build)
    monkeypatch.setattr(dev_workbench, "NATIVE_EXECUTION_HOST", native_host)
    monkeypatch.setattr(dev_workbench, "NATIVE_CANDIDATE_WORKER", candidate_worker)
    monkeypatch.setattr(dev_workbench, "_run", run)
    monkeypatch.setenv("MANYFORGE_DEV_BUILD_JOBS", "3")

    assert dev_workbench._ensure_current_native_backend() == (
        native_host,
        candidate_worker,
    )

    assert len(observed) == 2
    assert "-DBUILD_TESTING=OFF" in observed[0]
    assert observed[1][-5:] == (
        "manyforge_native_execution_host",
        "manyforge_roboplan_candidate_worker",
        "--",
        "-k",
        "0",
    )
    assert observed[1][observed[1].index("--parallel") + 1] == "3"


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
    assert (
        dev_workbench._manager_projection_ready(
            status,
            {**diff, "converged": False},
        )
        is False
    )
    assert (
        dev_workbench._manager_projection_ready(
            {**status, "status": {**status["status"], "manager_ready": False}},
            diff,
        )
        is False
    )


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
    launcher = (REPOSITORY_ROOT / "scripts/compose-dev.sh").read_text(encoding="utf-8")

    assert "docker image inspect -f '{{.Id}}'" in launcher
    assert 'cache_root="$cache_base/images/$image_key"' in launcher
    assert '"$image_id" \\' in launcher
    assert "MANYFORGE_DEV_QUALIFIED_BUILD_ROOT" not in launcher
    assert "/manyforge-qualified" not in launcher


def test_launcher_uses_optional_project_selection_not_catalog_ids() -> None:
    launcher = (REPOSITORY_ROOT / "scripts/compose-dev.sh").read_text(encoding="utf-8")

    assert "--project" in launcher
    assert "--scenario" not in launcher


def test_workbench_parser_allows_empty_or_project_startup() -> None:
    empty = dev_workbench._parser().parse_args(["serve"])
    selected = dev_workbench._parser().parse_args(
        ["serve", "--project", "examples/lite6_pick_and_place.scenario"]
    )

    assert empty.project is None
    assert selected.project == "examples/lite6_pick_and_place.scenario"


def test_project_status_reads_only_the_bounded_manifest_identity(tmp_path: Path) -> None:
    project = tmp_path / "cell.scenario"
    project.mkdir()
    (project / "scenario.yaml").write_text(
        '{"api_version":"manyforge.scenario.project.v2",'
        '"scenario_id":"cell"}',
        encoding="utf-8",
    )

    assert dev_workbench._project_scenario_id(
        str(project), allowed_root=tmp_path
    ) == "cell"


def test_workbench_selects_a_project_only_through_the_composer_loader() -> None:
    empty = dev_workbench.Workbench(
        project_path=None,
        headless_port=1,
        runner_port=2,
        composer_port=3,
        frontend_port=4,
    )
    selected = dev_workbench.Workbench(
        project_path="/workspace/examples/lite6_pick_and_place.scenario",
        headless_port=1,
        runner_port=2,
        composer_port=3,
        frontend_port=4,
    )

    assert "--startup-scenario-document" not in empty._composer_command("headless")
    command = selected._composer_command("headless")
    option = command.index("--startup-scenario-document")
    assert command[option + 1] == selected.project_path
    assert "--program-path" not in command


def test_cleanup_uses_bounded_reverse_owner_order(monkeypatch: Any) -> None:
    workbench = dev_workbench.Workbench(
        project_path=None,
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
                name for name, candidate in workbench.processes if candidate is process
            )
        ),
    )
    monkeypatch.setattr(workbench, "_stop_manager", lambda: observed.append("Manager"))
    monkeypatch.setattr(
        workbench,
        "_wait_for_program_runner_manager_expiry",
        lambda: observed.append("Program runner fenced"),
    )

    workbench.close()

    assert observed == [
        "Vite HMR",
        "Composer backend",
        "Manager",
        "Program runner fenced",
        "Program runner",
        "Headless API",
    ]
