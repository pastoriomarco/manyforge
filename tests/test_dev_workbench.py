from __future__ import annotations

import importlib.util
import hashlib
import json
import os
from pathlib import Path
import socket
import sys
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


def test_ros_home_is_owned_shared_and_inherited_before_service_spawn(
    tmp_path: Path, monkeypatch: Any
) -> None:
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.setenv("ROS_HOME", str(tmp_path / "inherited-home"))
    workbench = dev_workbench.Workbench(
        project_path=None, ros2_control=True,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    paths = workbench._paths()
    home = paths["execution_run"] / "ros-home"
    assert os.environ["ROS_HOME"] == str(home)
    assert home.is_dir()
    lock = home / "existing-spawner-lock"
    lock.write_text("same runtime", encoding="utf-8")

    # Reusing the same runtime must not choose a per-controller/epoch home or
    # rotate the lock directory. The actual spawn copies this environment.
    assert workbench._paths() == paths
    assert lock.read_text(encoding="utf-8") == "same runtime"
    result = tmp_path / "child-home.txt"
    workbench._spawn("environment fixture", (
        sys.executable, "-c",
        "import os,pathlib,sys; home=pathlib.Path(os.environ['ROS_HOME']); "
        "(home/'writable-child-probe').write_text('owned'); "
        "pathlib.Path(sys.argv[1]).write_text(str(home))",
        str(result),
    ))
    assert workbench.processes[-1][1].wait(timeout=10) == 0
    assert result.read_text(encoding="utf-8") == str(home)
    assert (home / "writable-child-probe").read_text(encoding="utf-8") == "owned"
    assert lock.read_text(encoding="utf-8") == "same runtime"
    assert not (tmp_path / "inherited-home").exists()


def test_repository_bootstrap_uses_the_exact_source_owned_generator(
    tmp_path: Path, monkeypatch: Any
) -> None:
    generator = tmp_path / "manyforge_generate_canonical_empty_package"
    generator.write_text("generator", encoding="utf-8")
    generator.chmod(0o755)
    bootstrap = tmp_path / "bootstrap"
    workspace = tmp_path / "workspace"
    runtime = "ros2_control"
    profile_relative = Path("manyforge_deployment/resources/maintained/catalog/"
                            f"runtime_profile/manyforge.{runtime}_execution_runtime.json")
    profile = workspace / profile_relative
    profile.parent.mkdir(parents=True)
    profile.write_bytes((REPOSITORY_ROOT / profile_relative).read_bytes())
    observed: list[tuple[str, ...]] = []

    def run(command: tuple[str, ...], **_kwargs: Any) -> None:
        observed.append(command)
        selected_root = Path(command[2])
        selected_root.mkdir(parents=True, exist_ok=True)
        (selected_root / "canonical-empty-admission.c1_build_authority_cell.json").write_text("{}")

    monkeypatch.setattr(dev_workbench, "CANONICAL_EMPTY_GENERATOR", generator)
    monkeypatch.setattr(dev_workbench, "WORKSPACE", workspace)
    monkeypatch.setattr(dev_workbench, "BOOTSTRAP_ROOT", bootstrap)
    monkeypatch.setattr(dev_workbench, "_run", run)

    def ensure() -> Path:
        return dev_workbench._ensure_repository_bootstrap_package()

    root = ensure()
    assert root.parent == bootstrap
    digest = hashlib.sha256(f"{runtime}-workbench:c1_build_authority_cell\0".encode())
    digest.update(generator.read_bytes() + b"\0" + profile.read_bytes() + b"\0")
    assert root.name == digest.hexdigest()
    assert observed == [
        (str(generator), "--output-root", str(root),
         "--cell-id", "c1_build_authority_cell",
         "--ros-runtime-profile",
         str(profile)),
    ]
    assert ensure() == root
    profile.write_bytes(profile.read_bytes() + b"\n")
    profile_root = ensure()
    assert profile_root != root
    generator.write_text("updated generator", encoding="utf-8")
    assert ensure() != profile_root
    assert (root / "canonical-empty-admission.c1_build_authority_cell.json").exists()


def test_status_reads_the_backend_derived_scenario_without_reducing_it_again(
    monkeypatch: Any,
) -> None:
    response = {
        "schema_version": "manyforge.current_scenario.v1",
        "ready": True,
        "current_scenario": {"scenario_id": "mock_lite6_pick_and_place"},
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
        scenario_id="mock_lite6_pick_and_place", composer_port=9020
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
        scenario_id="mock_lite6_pick_and_place", composer_port=9020
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
        ["serve", "--project", "examples/mock_lite6_pick_and_place.scenario"]
    )

    assert empty.project is None
    assert selected.project == "examples/mock_lite6_pick_and_place.scenario"
    assert not hasattr(empty, "ros2_control")
    with pytest.raises(SystemExit):
        dev_workbench._parser().parse_args(["serve", "--ros2-control"])


def test_ros_bootstrap_selection_leaves_the_composer_startup_empty() -> None:
    selected = dev_workbench._parser().parse_args([
        "serve", "--bootstrap-project", "examples/first.scenario",
    ])
    assert selected.project is None
    assert selected.bootstrap_project == "examples/first.scenario"
    with pytest.raises(SystemExit):
        dev_workbench._parser().parse_args([
            "serve", "--project", "one", "--bootstrap-project", "two",
        ])
    workbench = dev_workbench.Workbench(
        project_path=None, bootstrap_project_path=selected.bootstrap_project,
        ros2_control=True, headless_port=1, runner_port=2,
        composer_port=3, frontend_port=4,
    )
    assert "--startup-scenario-document" not in workbench._composer_command("http://headless")


def test_no_project_ros_start_selects_repository_empty_with_original_identity(
    monkeypatch: Any, tmp_path: Path
) -> None:
    monkeypatch.setenv("MANYFORGE_STATE_DIR", str(tmp_path / "installation-state"))
    monkeypatch.setattr(dev_workbench, "WORKSPACE", REPOSITORY_ROOT)
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.setattr(dev_workbench, "BOOTSTRAP_ROOT", tmp_path / "bootstrap")
    sources = {}
    for name in ("manyforge_manager_daemon", "manyforge_roboplan_candidate_worker",
                 "manyforge_ros2_control_execution_host", "generator", "preparer", "extension"):
        source = tmp_path / name
        source.write_bytes(name.encode())
        source.chmod(0o755)
        sources[name] = source
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", sources["manyforge_manager_daemon"])
    monkeypatch.setattr(dev_workbench, "CANONICAL_EMPTY_GENERATOR", sources["generator"])
    monkeypatch.setattr(dev_workbench, "_ensure_current_backend", lambda: sources["extension"])
    monkeypatch.setattr(dev_workbench, "_ensure_current_ros_backend", lambda: (
        sources["manyforge_roboplan_candidate_worker"], sources["manyforge_ros2_control_execution_host"]))
    monkeypatch.setattr(dev_workbench, "_ensure_activation_preparer", lambda: sources["preparer"])
    monkeypatch.setattr(dev_workbench, "_ensure_frontend_dependencies", lambda: None)
    monkeypatch.setattr(dev_workbench, "_port_available", lambda _port: True)
    monkeypatch.setattr(dev_workbench, "_capture_ros_bootstrap_project",
                        lambda *_a, **_k: pytest.fail("no-project startup has no Scenario to capture"))
    monkeypatch.setattr(dev_workbench, "_ensure_ros_bootstrap_package",
                        lambda *_a, **_k: pytest.fail("no-project startup uses the empty generator"))
    generator_commands = []

    def generate(command: tuple[str, ...]) -> None:
        generator_commands.append(command)
        (Path(command[2]) / dev_workbench.BOOTSTRAP_ADMISSION).write_text("{}")

    monkeypatch.setattr(dev_workbench, "_run", generate)
    workbench = dev_workbench.Workbench(
        project_path=None,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    commands = {}

    class PreparedStartup(Exception):
        pass

    def spawn(name: str, command: tuple[str, ...], **_kwargs) -> None:
        commands[name] = command
        if name == "Manager":
            raise PreparedStartup

    monkeypatch.setattr(workbench, "_spawn", spawn)
    monkeypatch.setattr(workbench, "_wait_until", lambda *_a, **_k: None)
    with pytest.raises(PreparedStartup):
        workbench.start()

    assert len(generator_commands) == 1
    generated = generator_commands[0]
    assert generated[3:] == (
        "--cell-id", "c1_build_authority_cell", "--ros-runtime-profile",
        str(REPOSITORY_ROOT / "manyforge_deployment/resources/maintained/catalog/"
            "runtime_profile/manyforge.ros2_control_execution_runtime.json"),
    )
    for name in ("Headless API", "Manager"):
        command = commands[name]
        assert command[command.index("--install-root") + 1] == generated[2]
        assert command[command.index("--canonical-empty-admission-filename") + 1] == (
            "canonical-empty-admission.c1_build_authority_cell.json")
    for name in ("Program runner", "Headless API"):
        command = commands[name]
        assert command[command.index("--cell-id") + 1] == "c1_build_authority_cell"
    installed = tmp_path / "state/current/installed-targets"
    assert {path.name for path in installed.iterdir()} == {
        "manyforge_manager_daemon", "manyforge_roboplan_candidate_worker",
        "manyforge_ros2_control_execution_host",
    }
    runner = commands["Program runner"]
    assert runner[runner.index("--cycle-journal-path") + 1] == str(tmp_path / "state/current/program-cycle.sqlite3")
    assert runner[runner.index("--snapshot-url") + 1] == (
        "http://127.0.0.1:3/api/scene/snapshot/render"
    )
    headless = commands["Headless API"]
    assert headless[headless.index("--historian-database-path") + 1] == str(tmp_path / "state/history.sqlite3")
    assert headless[headless.index("--projection-root") + 1] == str(tmp_path / "state/current/publications")
    manager_command = commands["Manager"]
    assert manager_command[manager_command.index("--logical-scene-database") + 1] == str(tmp_path / "state/current/logical-scene.sqlite3")
    assert manager_command[manager_command.index("--execution-state-root") + 1] == str(tmp_path / "state/current/execution-state")
    assert workbench.project_path is None and workbench.scenario_id is None
    assert "--startup-scenario-document" not in workbench._composer_command("http://headless")
    assert workbench.processes == []  # Every service spawn is mocked.


def test_process_stack_stages_only_selected_host_without_starting_services(
    monkeypatch: Any, tmp_path: Path
) -> None:
    from manyforge_headless_api.process_tests.test_process_stack import ProcessStack

    host = "manyforge_ros2_control_execution_host"
    selected_environment = "MANYFORGE_ROS2_CONTROL_EXECUTION_HOST"
    monkeypatch.delenv("MANYFORGE_ROS2_CONTROL_EXECUTION_HOST", raising=False)
    sources = {}
    for name in ("manyforge_manager_daemon", "manyforge_roboplan_candidate_worker", host):
        source = tmp_path / name
        source.write_bytes(name.encode())
        source.chmod(0o755)
        sources[name] = source
    monkeypatch.setenv(selected_environment, str(sources[host]))
    monkeypatch.setenv("MANYFORGE_ROBOPLAN_CANDIDATE_WORKER",
                       str(sources["manyforge_roboplan_candidate_worker"]))
    package_root = tmp_path / "package"
    package_root.mkdir()
    admission = "canonical-empty-admission.selected_cell.json"
    (package_root / admission).write_text("{}")
    stack = SimpleNamespace(root=tmp_path, package_root=package_root,
                            manager_executable=sources["manyforge_manager_daemon"])
    ProcessStack.prepare_native_package(stack, admission, ros2_control=True)
    installed = tmp_path / "installed-targets"
    assert {path.name for path in installed.iterdir()} == set(sources)
    for name, source in sources.items():
        assert (installed / name).read_bytes() == source.read_bytes()
        assert os.access(installed / name, os.X_OK)
    assert stack.manager_executable == installed / "manyforge_manager_daemon"
    assert stack.manager_package_root == package_root
    assert stack.manager_admission_filename == admission


@pytest.mark.parametrize("project,bootstrap,ros,error", [
    (None, "first", False, "requires ROS control"),
    ("first", "second", True, "excludes --project"),
    ("non_ros_fixture", None, True, "a ros2_control execution component"),
])
def test_invalid_ros_selection_is_refused_before_builds_or_runtime_start(
    monkeypatch: Any, project: str | None, bootstrap: str | None, ros: bool, error: str
) -> None:
    def unexpected_build() -> None:
        pytest.fail("invalid selection must fail before build")

    monkeypatch.setattr(dev_workbench, "_ensure_current_backend", unexpected_build)
    if project == "non_ros_fixture":
        # Maintained examples now use ROS. Supply an explicitly non-ROS
        # Scenario through the actual capture seam to retain this guard case.
        from manyforge_composer.backend import scenario_artifacts
        monkeypatch.setattr(scenario_artifacts, "capture_scenario_artifact",
            lambda *args, **kwargs: ({"deployment_draft": {"components": [
                {"configuration": {"runtime_kind": "native"}}]}}, project, "fixture_digest"))
    workbench = dev_workbench.Workbench(
        project_path=project, bootstrap_project_path=bootstrap,
        artifact_root=REPOSITORY_ROOT / "examples", ros2_control=ros,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    with pytest.raises(dev_workbench.WorkbenchError, match=error):
        workbench.start()
    assert workbench.processes == []


@pytest.mark.parametrize("kinds,admitted", [
    (["ros2_control"], True),
    (["ros2_control", "ros2_control"], True),
    (["pose_source", "ros2_control"], True),
    (["io_provider", "io_provider", "ros2_control"], True),
    (["ai_model", "pose_source", "ros2_control"], True),
    (["io_provider"], False),
    (["pose_source", "ai_model"], False),
    (["native"], False),
    (["native", "ros2_control"], False),
    (["unknown_kind", "ros2_control"], False),
])
def test_ros_start_guard_admits_model_free_components_beside_ros2_control(
    monkeypatch: Any, kinds: list[str], admitted: bool
) -> None:
    from manyforge_composer.backend import scenario_artifacts

    deployment = {"components": [
        {"configuration": {"runtime_kind": kind}} for kind in kinds]}
    # An incomplete fixture: past the guard, capture stops at the Program check.
    monkeypatch.setattr(scenario_artifacts, "capture_scenario_artifact",
        lambda *args, **kwargs: ({"deployment_draft": deployment,
                                  "program_draft": None}, "/fixture", "digest"))
    error = ("intended first ROS Scenario is incomplete" if admitted
             else "a ros2_control execution component")
    with pytest.raises(dev_workbench.WorkbenchError, match=error):
        dev_workbench._capture_ros_bootstrap_project(
            "/fixture", allowed_root=REPOSITORY_ROOT / "examples")


def test_ros_bootstrap_captures_io_provider_scenario() -> None:
    captured, source = dev_workbench._capture_ros_bootstrap_project(
        str(REPOSITORY_ROOT / "examples/real_lite6_pneumatic_pick_and_place.scenario"),
        allowed_root=REPOSITORY_ROOT / "examples",
    )
    assert source.endswith("/examples/real_lite6_pneumatic_pick_and_place.scenario")
    assert sorted(
        component["configuration"]["runtime_kind"]
        for component in captured["deployment"]["components"]
    ) == ["io_provider", "io_provider", "ros2_control"]


def test_ros_bootstrap_captures_authored_program_and_uses_production_preparer(
    monkeypatch: Any, tmp_path: Path
) -> None:
    from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
    from manyforge_headless_api import activation_preparer

    captured, source = dev_workbench._capture_ros_bootstrap_project(
        str(REPOSITORY_ROOT / "examples/sim_ur10e_pick_and_place.scenario"),
        allowed_root=REPOSITORY_ROOT / "examples",
    )
    assert source.endswith("/examples/sim_ur10e_pick_and_place.scenario")
    assert captured["program"]["api_version"] == "manyforge.program.v2"
    assert captured["model_source_closure"]["components"]
    assert sorted(
        component["configuration"]["runtime_kind"]
        for component in captured["deployment"]["components"]
    ) == ["pose_source", "ros2_control"]
    tool = tmp_path / "preparer"
    tool.write_bytes(b"current preparer")
    calls: list[tuple[dict[str, Any], dict[str, Any]]] = []

    class _Preparer:
        def __init__(self, **options: Any) -> None:
            self.options = options

        def prepare(self, **input: Any) -> dict[str, Any]:
            calls.append((self.options, input))
            (self.options["activation_root"] / dev_workbench.BOOTSTRAP_ADMISSION).write_text("fixture")
            return {}

    monkeypatch.setattr(activation_preparer, "ActivationPreparer", _Preparer)
    monkeypatch.setattr(dev_workbench, "WORKSPACE", REPOSITORY_ROOT)
    monkeypatch.setattr(dev_workbench, "BOOTSTRAP_ROOT", tmp_path / "bootstrap")
    root = dev_workbench._ensure_ros_bootstrap_package(captured, activation_preparer=tool)
    assert calls[0] == ({
        "executable": tool, "capability_root": REPOSITORY_ROOT,
        "activation_root": root, "runtime_cell_id": dev_workbench.WORKBENCH_CELL_ID,
        "timeout_seconds": 180, "publish_canonical_empty": True,
    }, {**captured, "captured_input_digest": raw_sha256_digest(canonical_json_bytes(captured))})
    assert dev_workbench._ensure_ros_bootstrap_package(captured, activation_preparer=tool) == root
    assert len(calls) == 2  # An existing admission still passes through its producer.
    tool.write_bytes(b"updated preparer")
    assert dev_workbench._ensure_ros_bootstrap_package(captured, activation_preparer=tool) != root


def test_ros_source_change_during_frontend_preparation_never_starts_runtime(
    monkeypatch: Any, tmp_path: Path
) -> None:
    captured = {"scenario_id": "first"}
    current_capture = captured

    def frontend_preparation() -> None:
        nonlocal current_capture
        current_capture = {"scenario_id": "changed"}

    monkeypatch.setattr(
        dev_workbench, "_capture_ros_bootstrap_project",
        lambda *_a, **_k: (current_capture, "/project"),
    )
    monkeypatch.setattr(dev_workbench, "_port_available", lambda _port: True)
    manager = tmp_path / "manager"
    manager.write_bytes(b"current Manager")
    manager.chmod(0o755)
    (tmp_path / "extension").write_bytes(b"current extension")
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", manager)
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.setattr(dev_workbench, "_ensure_frontend_dependencies", frontend_preparation)
    monkeypatch.setattr(dev_workbench, "_ensure_current_backend", lambda: tmp_path / "extension")
    monkeypatch.setattr(dev_workbench, "_ensure_current_ros_backend", lambda: ())
    monkeypatch.setattr(dev_workbench, "_ensure_activation_preparer", lambda: tmp_path / "preparer")
    monkeypatch.setattr(dev_workbench, "_ensure_ros_bootstrap_package", lambda *_a, **_k: tmp_path)
    workbench = dev_workbench.Workbench(
        project_path=None, bootstrap_project_path="/project", ros2_control=True,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    with pytest.raises(dev_workbench.WorkbenchError, match="changed during startup preparation"):
        workbench.start()
    assert workbench.processes == []


def test_runtime_capability_selection_keeps_one_build_closure(
    monkeypatch: Any, tmp_path: Path
) -> None:
    ros = (tmp_path / "worker", tmp_path / "ros")
    calls: list[str] = []
    monkeypatch.setattr(
        dev_workbench, "_ensure_current_ros_backend",
        lambda: calls.append("ros") or ros,
    )
    workbench = dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2,
        composer_port=3, frontend_port=4,
    )

    assert workbench.ros2_control is True
    assert workbench._runtime_targets() == ros
    assert calls == ["ros"]
    monkeypatch.setattr(dev_workbench, "WORKSPACE", REPOSITORY_ROOT)
    assert workbench._simulation_control_arguments() == (
        ("--simulation-control-config", str(REPOSITORY_ROOT /
         "manyforge_ros_bringup/config/isaac_program_simulation_site.json"))
    )


@pytest.mark.parametrize("build_testing", [None, "ON"])
def test_ros_build_stages_current_products_and_imports_overlay_before_spawn(
    monkeypatch: Any, tmp_path: Path, build_testing: str | None
) -> None:
    build_root = tmp_path / "build"
    ros_build = build_root / "ros-integration"
    overlay = build_root / "isaac/ros-overlay/install/setup.bash"
    commands: list[tuple[str, ...]] = []
    parent_environment = {**os.environ, "ROS_STATIC_PEERS": "orin;laptop",
                          "RMW_IMPLEMENTATION": "rmw_fastrtps_cpp"}
    monkeypatch.setattr(dev_workbench.os, "environ", parent_environment)
    monkeypatch.setattr(dev_workbench, "BUILD_ROOT", build_root)
    monkeypatch.setattr(dev_workbench, "WORKSPACE", REPOSITORY_ROOT)
    monkeypatch.setenv("MANYFORGE_DEV_BUILD_JOBS", "2")
    monkeypatch.delenv("MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS", raising=False)
    monkeypatch.delenv("MANYFORGE_ROS2_CONTROL_BUILD_TESTING", raising=False)
    if build_testing is not None:
        monkeypatch.setenv("MANYFORGE_ROS2_CONTROL_BUILD_TESTING", build_testing)

    def build(command: tuple[str, ...], *, env: dict[str, str]) -> None:
        commands.append(command)
        assert env["ROS_STATIC_PEERS"] == "orin;laptop"
        assert env["MANYFORGE_ROBOPLAN_BUILD_DIR"] == str(ros_build)
        assert env["MANYFORGE_ROS2_CONTROL_BUILD_TESTING"] == (build_testing or "OFF")
        assert env["CMAKE_BUILD_PARALLEL_LEVEL"] == "2"
        assert env["CMAKE_GENERATOR"] == "Ninja"
        ros_build.mkdir(parents=True, exist_ok=True)
        targets = ("manyforge_ros2_control_execution_host", "manyforge_roboplan_candidate_worker")
        for name in targets:
            source = ros_build / name
            source.write_bytes(name.encode())
            source.chmod(0o755)
        overlay.parent.mkdir(parents=True, exist_ok=True)
        overlay.write_text("overlay", encoding="utf-8")

    def source_overlay(command: tuple[str, ...], **kwargs: Any) -> Any:
        assert command[-1] == str(overlay)
        assert kwargs["env"]["ROS_STATIC_PEERS"] == "orin;laptop"
        return SimpleNamespace(
            returncode=0, stdout=b"AMENT_PREFIX_PATH=/current/overlay\0PYTHONPATH=/current/messages\0"
        )

    monkeypatch.setattr(dev_workbench, "_run", build)
    monkeypatch.setattr(dev_workbench.subprocess, "run", source_overlay)
    targets = dev_workbench._ensure_current_ros_backend()
    assert commands == [(
        str(REPOSITORY_ROOT / "scripts/ros/build-ros2-control-runtime.sh"),
        "--source-dir", str(REPOSITORY_ROOT / "scripts/ros/ros-integration"),
    )]
    assert [target.name for target in targets] == [
        "manyforge_roboplan_candidate_worker",
        "manyforge_ros2_control_execution_host",
    ]
    installed = tmp_path / "installed-targets"
    installed.mkdir()
    dev_workbench._stage_runtime_targets(targets, installed)
    for target in targets:
        assert (installed / target.name).read_bytes() == target.name.encode()
        assert os.access(installed / target.name, os.X_OK)
    assert not (installed / "manyforge_execution_host").exists()
    # The adapter families' index prefix comes first.
    ament_prefix = f"{ros_build / 'ament_cmake_index'}{os.pathsep}/current/overlay"
    assert parent_environment["AMENT_PREFIX_PATH"] == ament_prefix
    assert parent_environment["ROS_STATIC_PEERS"] == "orin;laptop"

    spawned: list[dict[str, str]] = []
    monkeypatch.setattr(
        dev_workbench.subprocess, "Popen",
        lambda _command, **kwargs: spawned.append(kwargs["env"]) or SimpleNamespace(),
    )
    workbench = dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2,
        composer_port=3, frontend_port=4, ros2_control=True,
    )
    workbench._spawn("Manager", ("manager",))
    assert spawned[0]["AMENT_PREFIX_PATH"] == ament_prefix
    assert spawned[0]["RMW_IMPLEMENTATION"] == "rmw_fastrtps_cpp"


def test_project_status_reads_only_the_bounded_manifest_identity(tmp_path: Path) -> None:
    project = tmp_path / "My_Cell-2.scenario"
    project.mkdir()
    (project / "scenario.yaml").write_text(
        '{"api_version":"manyforge.scenario.project.v3",'
        '"scenario_id":"My_Cell-2","name":"My_Cell-2"}',
        encoding="utf-8",
    )

    assert dev_workbench._project_scenario_id(
        str(project), allowed_root=tmp_path
    ) == "My_Cell-2"


def test_project_status_refuses_legacy_v2_project(tmp_path: Path) -> None:
    project = tmp_path / "legacy.scenario"
    project.mkdir()
    (project / "scenario.yaml").write_text(
        '{"api_version":"manyforge.scenario.project.v2",'
        '"scenario_id":"legacy","name":"legacy"}', encoding="utf-8"
    )
    with pytest.raises(dev_workbench.WorkbenchError, match="re-author.*direct references"):
        dev_workbench._project_scenario_id(str(project), allowed_root=tmp_path)


def test_composer_roots_include_launcher_mounted_folders(
    monkeypatch: Any, tmp_path: Path
) -> None:
    examples = tmp_path / "examples"
    monkeypatch.delenv("MANYFORGE_DEV_PROJECT_ROOTS", raising=False)
    assert dev_workbench._composer_filesystem_roots(examples) == str(examples)
    monkeypatch.setenv(
        "MANYFORGE_DEV_PROJECT_ROOTS",
        "/projects/first,/projects/second",
    )
    assert dev_workbench._composer_filesystem_roots(examples) == (
        f"{examples},/projects/first,/projects/second"
    )


@pytest.mark.parametrize(
    ("folder", "scenario_id", "name"),
    [
        ("legacy.cell.scenario", "legacy.cell", "legacy.cell"),
        ("cell", "cell", "cell"),
        ("cell.scenario", "other", "other"),
        ("cell.scenario", "cell", "Cell title"),
    ],
)
def test_project_status_refuses_names_outside_the_one_name_rule(
    tmp_path: Path, folder: str, scenario_id: str, name: str
) -> None:
    project = tmp_path / folder
    project.mkdir()
    (project / "scenario.yaml").write_text(
        '{"api_version":"manyforge.scenario.project.v3",'
        f'"scenario_id":"{scenario_id}","name":"{name}"}}',
        encoding="utf-8",
    )

    with pytest.raises(dev_workbench.WorkbenchError, match="<name>.scenario"):
        dev_workbench._project_scenario_id(str(project), allowed_root=tmp_path)


def test_workbench_selects_a_project_only_through_the_composer_loader() -> None:
    empty = dev_workbench.Workbench(
        project_path=None,
        headless_port=1,
        runner_port=2,
        composer_port=3,
        frontend_port=4,
    )
    selected = dev_workbench.Workbench(
        project_path="/workspace/examples/mock_lite6_pick_and_place.scenario",
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


def test_g_static_composer_preserves_endpoint(tmp_path):
    static = tmp_path / "frontend"
    static.mkdir()
    (static / "index.html").write_text("built fixture frontend")
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=True,
        headless_port=9010, runner_port=9011, composer_port=9000, frontend_port=3000,
        composer_static_dir=static, composer_bind_host="0.0.0.0")
    command = workbench._composer_command("http://127.0.0.1:9010")
    assert command[command.index("--host") + 1] == "0.0.0.0"
    assert command[command.index("--port") + 1] == "9000"
    assert command[command.index("--static-dir") + 1] == str(static)


def test_site_settings_forward_only_when_configured(tmp_path):
    site = tmp_path / "manyforge.site.json"
    site.write_text('{"schema":"manyforge.site.v1","device_addresses":{}}')
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=True,
        headless_port=9010, runner_port=9011, composer_port=9020, frontend_port=3000,
        site_settings=site)
    assert workbench._site_manager_arguments() == ("--site-settings", str(site))
    site.unlink()
    with pytest.raises(dev_workbench.WorkbenchError, match="site settings"):
        workbench._site_manager_arguments()
    workbench.site_settings = None
    assert workbench._site_manager_arguments() == ()
    args = dev_workbench._parser().parse_args(["serve", "--site-settings", str(site)])
    assert args.site_settings == site
    assert dev_workbench._parser().parse_args(["serve"]).site_settings is None


@pytest.mark.parametrize("fresh,manager_ready,expected", [(True, True, True), (False, True, False), (True, False, False)])
def test_g_status_uses_fresh_manager_health_without_cccl_apply_permission(tmp_path, monkeypatch, fresh, manager_ready, expected):
    manager = {"source": {"available": True, "freshness": "fresh" if fresh else "stale", "known_gaps": []},
               "status": {"manager_ready": manager_ready, "components": [{"lifecycle_state": "inactive"}]}}
    class Client:
        def __init__(self, url, **kwargs):
            self.url = url
        def __enter__(self):
            return self
        def __exit__(self, *args):
            pass
        def manager_status(self):
            return manager
        def health(self):
            return {"status": "ready", "headless_projection_ready": True}
        def request(self, method, path):
            assert (method, path) == ("GET", "/api/program/health")
            return {"started": True, "closed": False}
    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", Client)
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path)
    (tmp_path / "current").mkdir()
    (tmp_path / "current/workbench-startup-timing.json").write_text('{"build_elapsed_seconds":999,"readiness_elapsed_seconds":null}')
    status = dev_workbench.query_operator_status(headless_port=9010, runner_port=9011, composer_port=9000)
    assert status["ready"] is expected
    assert status["manager"] == manager
    assert status["timing"]["build_elapsed_seconds"] == 999


def test_g_package_mismatch_refuses_before_build_or_process_start(tmp_path, monkeypatch):
    from manyforge_operator.package_aggregate import package_aggregate_sha256
    install = tmp_path / "package"
    install.mkdir()
    (install / "empty.json").write_text("{}")
    digest = package_aggregate_sha256(install)
    (install / "empty.json").write_text('{"changed":true}')
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=True,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
        operator_install_root=install, operator_cell_id="fixture_cell",
        canonical_empty_admission_filename="empty.json", package_aggregate_digest=digest)
    monkeypatch.setattr(dev_workbench, "_ensure_current_backend", lambda: pytest.fail("must not build changed package"))
    with pytest.raises(dev_workbench.WorkbenchError, match="aggregate mismatch"):
        workbench.start()
    assert workbench.processes == []


def test_g_owner_start(tmp_path, monkeypatch):
    from manyforge_operator.package_aggregate import package_aggregate_sha256
    monkeypatch.setenv("MANYFORGE_STATE_DIR", str(tmp_path / "installation-state"))
    install = tmp_path / "package"
    install.mkdir()
    (install / "empty.json").write_text("{}")
    manager_binary = tmp_path / "manager"
    manager_binary.write_bytes(b"fixture executable")
    manager_binary.chmod(0o755)
    extension = tmp_path / "extension.so"
    extension.write_bytes(b"fixture extension")
    clock = [0.0]
    session_started_ns = 1791900000000000000
    monkeypatch.setattr(dev_workbench.time, "time_ns", lambda: session_started_ns)
    monkeypatch.setenv("ROS_HOME", str(tmp_path / "foreign-home"))
    monkeypatch.setenv("ROS_LOG_DIR", str(tmp_path / "foreign-logs"))
    def backend():
        clock[0] += 120.0  # Arbitrary fixture duration, not a runtime claim.
        return extension
    monkeypatch.setattr(dev_workbench.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", manager_binary)
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.setattr(dev_workbench, "_ensure_current_backend", backend)
    monkeypatch.setattr(dev_workbench, "_ensure_current_ros_backend", lambda: ())
    monkeypatch.setattr(dev_workbench, "_ensure_activation_preparer", lambda: tmp_path / "preparer")
    monkeypatch.setattr(dev_workbench, "_port_available", lambda port: port != 3000)
    monkeypatch.setattr(dev_workbench, "_ensure_frontend_dependencies", lambda: pytest.fail("G must not npm ci"))
    monkeypatch.setattr(dev_workbench, "_ensure_repository_bootstrap_package", lambda: pytest.fail("G must use its qualified package"))
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=True,
        headless_port=9010, runner_port=9011, composer_port=9000, frontend_port=3000,
        operator_install_root=install, operator_cell_id="fixture_cell",
        operator_instance_id="fixture-instance", canonical_empty_admission_filename="empty.json",
        package_aggregate_digest=package_aggregate_sha256(install), operator_readiness_timeout=5,
        composer_bind_host="0.0.0.0")
    monkeypatch.setattr(workbench, "_simulation_control_arguments", lambda: ())
    commands = []
    bridge_environments: list[dict[str, str]] = []
    socket_owner = socket.socket(socket.AF_UNIX)
    manager_environments: list[dict[str, str] | None] = []
    def spawn(name, command, environment=None):
        if name == "Manager":
            manager_environments.append(environment)
        logs = tmp_path / "state/current/execution-run/ros" / f"1-{session_started_ns}"
        assert os.environ["ROS_LOG_DIR"] == str(logs)
        assert logs.is_dir()
        assert os.environ["ROS_HOME"] == str(tmp_path / "state/current/execution-run/ros-home")
        commands.append((name, tuple(command)))
        workbench.processes.append((name, SimpleNamespace(poll=lambda: None)))
        if name == "Manager":
            index = command.index("--manager-run-root")
            socket_owner.bind(str(Path(command[index + 1]) / "manager-command.sock"))
    monkeypatch.setattr(workbench, "_spawn", spawn)
    def popen(command, **kwargs):
        name = (
            "Assistant bridge"
            if "manyforge_assistant_bridge.bridge" in command
            else "Composer backend"
        )
        commands.append((name, tuple(command)))
        if name == "Assistant bridge":
            bridge_environments.append(kwargs["env"])
        return SimpleNamespace(poll=lambda: None)
    monkeypatch.setattr(dev_workbench.subprocess, "Popen", popen)
    class Client:
        def __init__(self, *args, **kwargs):
            pass
        def __enter__(self):
            return self
        def __exit__(self, *args):
            pass
        def request(self, *args):
            return {"started": True}
        def runtime(self):
            return {"schema_version": "fixture"}
        def manager_status(self):
            return {"source": {"available": True, "freshness": "fresh", "known_gaps": []},
                    "status": {"manager_ready": True}}
        def manager_diff(self):
            pytest.fail("G must not wait for Apply convergence")
        def program_cycle_status(self):
            pytest.fail("G READY must not wait for Program containment")
        def authoring_admission(self):
            pytest.fail("G READY must not imply authoring permission")
        def health(self):
            return {"status": "ready"}
    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", Client)
    try:
        workbench.start()
    finally:
        socket_owner.close()
    assert [name for name, _ in commands] == [
        "Program runner", "Headless API", "Manager", "Assistant bridge", "Composer backend",
    ]
    composer_command = dict(commands)["Composer backend"]
    assert composer_command[composer_command.index("--assistant-bridge-url") + 1] == (
        "http://127.0.0.1:8100"
    )
    (bridge_environment,) = bridge_environments
    assert bridge_environment["MANYFORGE_HEADLESS_API_URL"] == "http://127.0.0.1:9010"
    assert bridge_environment["MANYFORGE_COMPOSER_URL"] == "http://127.0.0.1:9000"
    # The execution host (and a point-cloud source's process) inherit it.
    assert manager_environments == [{"MANYFORGE_COMPOSER_URL": "http://127.0.0.1:9000"}]
    assert bridge_environment["MANYFORGE_ASSISTANT_BRIDGE_PORT"] == "8100"
    assert bridge_environment["MANYFORGE_ASSISTANT_AUDIT_LOG"] == str(
        tmp_path / "state/current/assistant-bridge/audit.jsonl"
    )
    assert not any(key.startswith("MANYFORGE_ASSISTANT_UPSTREAM") for key in bridge_environment)
    assert workbench._readiness_deadline == 125.0  # Build duration is outside readiness budget.
    timing = json.loads((tmp_path / "state/current/workbench-startup-timing.json").read_text())
    assert timing == {"build_elapsed_seconds": 120.0, "readiness_elapsed_seconds": 0.0}
    assert os.environ["ROS_LOG_DIR"] == str(tmp_path / "state/current/execution-run/ros" / f"1-{session_started_ns}")
    assert not (tmp_path / "foreign-home").exists()
    assert not (tmp_path / "foreign-logs").exists()
    for name, command in commands[:2]:
        assert command[command.index("--cell-id") + 1] == "fixture_cell"
        assert command[command.index("--owner-id") + 1].endswith("fixture-instance")
    manager_command = dict(commands)["Manager"]
    assert manager_command[manager_command.index("--install-root") + 1] == str(install)
    assert manager_command[manager_command.index("--canonical-empty-admission-filename") + 1] == "empty.json"


def test_restored_terminal_cycle_starts_vite_without_reset_or_authoring_admission(
    tmp_path: Path, monkeypatch: Any,
) -> None:
    monkeypatch.setenv("MANYFORGE_STATE_DIR", str(tmp_path / "installation-state"))
    manager_binary = tmp_path / "manager"
    manager_binary.write_bytes(b"fixture executable")
    manager_binary.chmod(0o755)
    extension = tmp_path / "extension.so"
    extension.write_bytes(b"fixture extension")
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", manager_binary)
    monkeypatch.setattr(dev_workbench, "_ensure_current_backend", lambda: extension)
    monkeypatch.setattr(dev_workbench, "_ensure_current_ros_backend", lambda: ())
    monkeypatch.setattr(dev_workbench, "_ensure_activation_preparer", lambda: tmp_path / "preparer")
    monkeypatch.setattr(dev_workbench, "_ensure_repository_bootstrap_package", lambda: tmp_path / "package")
    monkeypatch.setattr(dev_workbench, "_ensure_frontend_dependencies", lambda: None)
    monkeypatch.setattr(dev_workbench, "_port_available", lambda _port: True)
    monkeypatch.setenv("ROS_HOME", str(tmp_path / "inherited-home"))
    monkeypatch.setenv("ROS_LOG_DIR", str(tmp_path / "inherited-logs"))
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=True,
        headless_port=9010, runner_port=9011, composer_port=9020, frontend_port=3000)
    monkeypatch.setattr(workbench, "_simulation_control_arguments", lambda: ())
    spawned: list[str] = []
    def spawn(name, _command, **_kwargs):
        spawned.append(name)
        workbench.processes.append((name, SimpleNamespace(poll=lambda: None)))
    monkeypatch.setattr(workbench, "_spawn", spawn)
    def popen(command, **_kwargs):
        spawned.append(
            "Vite HMR" if command[0] == "node"
            else "Assistant bridge" if "manyforge_assistant_bridge.bridge" in command
            else "Composer backend"
        )
        return SimpleNamespace(poll=lambda: None)
    monkeypatch.setattr(dev_workbench.subprocess, "Popen", popen)
    cycle = {"state": "terminal_awaiting_reset"}
    requests: list[tuple[str, str]] = []
    class Client:
        def __init__(self, *_args, **_kwargs):
            pass
        def __enter__(self):
            return self
        def __exit__(self, *_args):
            pass
        def request(self, method, path):
            requests.append((method, path))
            assert (method, path) == ("GET", "/api/program/health")
            return {"started": True}
        def runtime(self):
            return {"schema_version": "fixture"}
        def manager_status(self):
            return {"source": {"available": True, "freshness": "fresh", "known_gaps": []},
                    "status": {"manager_ready": True, "runtime": {"converged": True}}}
        def manager_diff(self):
            return {"source": {"available": True, "freshness": "fresh", "known_gaps": []},
                    "converged": True}
        def program_cycle_status(self):
            return dict(cycle)
        def authoring_admission(self):
            pytest.fail("UI startup must not wait for idle command admission")
        def health(self):
            return {"status": "ready"}
    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", Client)
    waits: list[str] = []
    def wait(description, predicate, _processes, **_kwargs):
        waits.append(description)
        # Process/socket mechanics are substituted; owner readiness and
        # contained-cycle predicates still run through the real start path.
        if description in {"Manager command socket", "Vite HMR"}:
            return True
        result = predicate()
        assert result
        return result
    monkeypatch.setattr(workbench, "_wait_until", wait)

    workbench.start()

    assert spawned == [
        "Program runner", "Headless API", "Manager", "Assistant bridge", "Composer backend", "Vite HMR",
    ]
    assert waits == ["Program Runner and Headless reconciliation services", "Manager command socket",
                     "contained Program-cycle startup reconciliation", "Composer backend", "Vite HMR"]
    assert requests == [("GET", "/api/program/health")]
    assert cycle == {"state": "terminal_awaiting_reset"}


def test_direct_serve_selects_one_session_before_starting_any_owner(tmp_path, monkeypatch):
    root = tmp_path / "state"
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", root)
    monkeypatch.delenv("MANYFORGE_DEV_COLD_SESSION_ID", raising=False)
    monkeypatch.setattr(dev_workbench.signal, "signal", lambda *_args: None)
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=False,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4)
    started = []

    def start():
        assert workbench.session is not None
        marker = json.loads((root / "current/session.json").read_text())
        assert marker["session_id"] == workbench.session.marker["session_id"]
        assert marker["launcher_claimed"] is True
        assert workbench.runtime_root == root / "current"
        assert not (root / "previous-sessions").exists()
        started.append(marker["session_id"])
        workbench.stop_requested = True

    monkeypatch.setattr(workbench, "start", start)
    monkeypatch.setattr(workbench, "_stop_manager", lambda: True)
    monkeypatch.setattr(workbench, "_wait_for_program_runner_manager_expiry", lambda: True)
    workbench.serve()
    assert len(started) == 1
    assert workbench.session is None
    marker = json.loads((root / "current/session.json").read_text())
    assert marker["session_id"] == started[0] and marker["disposition"] == "stopped"


def test_preselected_session_report_is_shared_by_existing_consumers(tmp_path, monkeypatch):
    root = tmp_path / "state"
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", root)
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=False,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4)
    assert workbench._previous_session_report() is None
    assert workbench._previous_session_arguments() == ()
    previous = "a" * 32
    workbench.session = SimpleNamespace(marker={"previous_session": {"session_id": previous}})
    report = root / "session-reports" / (previous + ".json")
    assert workbench._previous_session_report() == report
    assert workbench._previous_session_arguments() == ("--workbench-previous-session-reports-root", str(root / "session-reports"), "--workbench-previous-session-id", previous)


@pytest.mark.parametrize("dependencies", [False, True])
def test_ros_dependency_setups_precede_build_and_application_overlay_is_last(
    tmp_path: Path, monkeypatch: Any, dependencies: bool
) -> None:
    # Import actual setup scripts with literal arguments, including shell
    # metacharacters in a filename. The build/service owners are not started.
    monkeypatch.setattr(dev_workbench.os, "environ", dict(os.environ))
    monkeypatch.delenv("MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS", raising=False)
    monkeypatch.delenv("MANYFORGE_SETUP_ORDER", raising=False)
    build = tmp_path / "build"
    monkeypatch.setattr(dev_workbench, "BUILD_ROOT", build)
    monkeypatch.setenv("MANYFORGE_DEV_BUILD_JOBS", "1")
    if dependencies:
        first = tmp_path / "first $literal.bash"
        second = tmp_path / "second.bash"
        first.write_text('export MANYFORGE_SETUP_ORDER="first"\nprintf "setup output"\n')
        second.write_text('export MANYFORGE_SETUP_ORDER="${MANYFORGE_SETUP_ORDER}:second"\n')
        monkeypatch.setenv("MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS", f"{first}:{second}")
    seen = []

    def run(command: tuple[str, ...], *, env: dict[str, str]) -> None:
        seen.append((command, dict(env)))
        assert env.get("MANYFORGE_SETUP_ORDER") == ("first:second" if dependencies else None)
        runtime = build / "ros-integration"
        runtime.mkdir(parents=True)
        for name in ("manyforge_ros2_control_execution_host", "manyforge_roboplan_candidate_worker"):
            target = runtime / name
            target.write_text("fixture")
            target.chmod(0o755)
        overlay = build / "isaac/ros-overlay/install/setup.bash"
        overlay.parent.mkdir(parents=True)
        overlay.write_text('export MANYFORGE_SETUP_ORDER="${MANYFORGE_SETUP_ORDER:+${MANYFORGE_SETUP_ORDER}:}application"\n')

    monkeypatch.setattr(dev_workbench, "_run", run)
    dev_workbench._ensure_current_ros_backend()
    assert len(seen) == 1
    assert seen[0][0][0].endswith("scripts/ros/build-ros2-control-runtime.sh")
    assert os.environ["MANYFORGE_SETUP_ORDER"] == (
        "first:second:application" if dependencies else "application")


@pytest.mark.parametrize("configured", ["missing", "relative", "failed"])
def test_ros_dependency_setup_failure_prevents_build_with_exact_path(
    tmp_path: Path, monkeypatch: Any, configured: str
) -> None:
    monkeypatch.setattr(dev_workbench.os, "environ", dict(os.environ))
    setup = tmp_path / "configured.bash"
    if configured == "failed":
        setup.write_text("return 23\n")
    selected = str(setup) if configured != "relative" else "relative.bash"
    monkeypatch.setenv("MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS", selected)
    monkeypatch.setattr(dev_workbench, "_run", lambda *_args, **_kwargs: pytest.fail("build started"))
    with pytest.raises(dev_workbench.WorkbenchError) as raised:
        dev_workbench._ensure_current_ros_backend()
    assert selected in str(raised.value)
