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


def _built_native_fixture(
    monkeypatch: Any, *, extension: Path, preparer: Path, runtime_targets: tuple[Path, ...] = ()
) -> None:
    """serve only checks the native artifacts; it never runs a build step."""

    for build in ("_ensure_current_ros_backend", "_ensure_activation_preparer"):
        monkeypatch.setattr(dev_workbench, build,
                            lambda *_a, _build=build, **_k: pytest.fail(f"serve ran {_build}"))
    monkeypatch.setattr(dev_workbench, "_current_backend", lambda: extension)
    monkeypatch.setattr(dev_workbench, "_ros_backend_environment", dict)
    monkeypatch.setattr(dev_workbench, "_current_ros_backend", lambda _environment: runtime_targets)
    monkeypatch.setattr(dev_workbench, "_current_activation_preparer", lambda: preparer)
    monkeypatch.setattr(dev_workbench, "_current_core_bindings", lambda: extension)


class PreparedStartup(Exception):
    """Raised instead of starting the Manager: the session is fully prepared."""


_PROCESS_LABELS = {
    "headless": "Headless API",
    "program_runner": "Program runner",
    "composer": "Composer backend",
}


def _prepared_start(workbench: Any, monkeypatch: Any) -> tuple[dict[str, tuple[str, ...]], dict[str, Any]]:
    """Run start() up to the Manager launch; return every argv and the list."""

    captured: dict[str, Any] = {}

    def spawn_manager(command: tuple[str, ...], *, environment: dict[str, str]) -> None:
        captured["command"] = tuple(command)
        captured["environment"] = environment
        raise PreparedStartup

    monkeypatch.setattr(workbench, "_spawn_manager", spawn_manager)
    monkeypatch.setattr(dev_workbench.shutil, "which",
                        lambda name: "/usr/bin/node" if name == "node" else None)
    with pytest.raises(PreparedStartup):
        workbench.start()
    manager = captured["command"]
    document = json.loads(
        Path(manager[manager.index("--application-processes") + 1]).read_text(encoding="utf-8")
    )
    commands = {"Manager": manager}
    for row in document["processes"]:
        commands[_PROCESS_LABELS[row["role"]]] = tuple(row["argv"])
    return commands, {"document": document, "manager_environment": captured["environment"]}


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
    # The Manager inherits it, and every application process inherits it
    # from the Manager.
    workbench._spawn_manager((
        sys.executable, "-c",
        "import os,pathlib,sys; home=pathlib.Path(os.environ['ROS_HOME']); "
        "(home/'writable-child-probe').write_text('owned'); "
        "pathlib.Path(sys.argv[1]).write_text(str(home))",
        str(result),
    ), environment={})
    assert workbench.manager.wait(timeout=10) == 0
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


def _no_project_ros_workbench(
    monkeypatch: Any, tmp_path: Path, **options: Any
) -> tuple[Any, list[tuple[str, ...]]]:
    """A no-project ROS Workbench whose native artifacts are fixtures."""

    monkeypatch.setenv("MANYFORGE_STATE_DIR", str(tmp_path / "installation-state"))
    for variable in ("MANYFORGE_LOG_EVIDENCE_MAX_DISK_PERCENT", "MANYFORGE_LOG_EVIDENCE_MIN_FREE_BYTES"):
        monkeypatch.delenv(variable, raising=False)
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
    _built_native_fixture(monkeypatch, extension=sources["extension"], preparer=sources["preparer"],
        runtime_targets=(sources["manyforge_roboplan_candidate_worker"],
                         sources["manyforge_ros2_control_execution_host"]))
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
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4, **options,
    )
    return workbench, generator_commands


def test_no_project_ros_start_selects_repository_empty_with_original_identity(
    monkeypatch: Any, tmp_path: Path
) -> None:
    workbench, generator_commands = _no_project_ros_workbench(monkeypatch, tmp_path)
    commands, prepared = _prepared_start(workbench, monkeypatch)

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
    assert headless[headless.index("--historian-directory") + 1] == str(tmp_path / "state/history")
    # The disk check's floor (10 GB plus the machine minimum 0), margin and
    # record folders (History, and current/ above the execution run root).
    assert headless[headless.index("--disk-free-floor-bytes") + 1] == "10000000000"
    assert headless[headless.index("--disk-free-margin-bytes") + 1] == "1000000000"
    assert headless[headless.index("--execution-run-root") + 1] == str(tmp_path / "state/current/execution-run")
    # OD-216: the Manager's state comes over its status socket in the Manager
    # run root; no status folder or option remains.
    assert headless[headless.index("--manager-run-root") + 1] == str(tmp_path / "state/current/manager-run")
    assert "--projection-root" not in headless
    assert "--projection-root" not in commands["Manager"]
    assert not (tmp_path / "state/current/publications").exists()
    manager_command = commands["Manager"]
    assert manager_command[manager_command.index("--execution-state-root") + 1] == str(tmp_path / "state/current/execution-state")
    assert workbench.project_path is None and workbench.scenario_id is None
    assert "--startup-scenario-document" not in workbench._composer_command("http://headless")
    assert workbench.manager is None  # The Manager launch is mocked.
    # One supervisor: the launcher starts only the Manager and hands it the
    # ordered list; Composer starts after the first converged
    # baseline (spec 3.3, 4.7). No launcher-composed owner ids (spec 12).
    document = prepared["document"]
    assert document["api_version"] == "manyforge.application_processes.v1"
    # Contract 1.1: only the three roles; Vite stays the launcher's own child.
    assert set(document) == {"api_version", "processes"}
    assert all(set(row) == {"role", "argv", "environment", "start"} for row in document["processes"])
    assert [(row["role"], row["start"]) for row in document["processes"]] == [
        ("headless", "immediate"),
        ("program_runner", "immediate"),
        ("composer", "after_baseline"),
    ]
    assert workbench._vite_command == (
        "/usr/bin/node", str(dev_workbench.FRONTEND_ROOT / "node_modules/vite/bin/vite.js"),
        "--host", "0.0.0.0", "--port", "4")
    # Composer and Headless read below the same folders: the examples
    # (Composer's default browse folder), the mounted project folders and
    # ManyForge's own content root, the workbench folder (OD-335).
    read_roots = document["processes"][2]["environment"]["MANYFORGE_FS_ROOTS"]
    assert read_roots.split(",")[0] == str(dev_workbench.WORKSPACE / "examples")
    assert read_roots.split(",")[-1] == str(dev_workbench.WORKSPACE)
    assert document["processes"][0]["environment"] == {"MANYFORGE_FS_ROOTS": read_roots}
    assert "--scene-asset-root" not in commands["Manager"]
    assert all("--owner-id" not in row["argv"] for row in document["processes"])
    assert prepared["manager_environment"] == {
        "MANYFORGE_COMPOSER_URL": "http://127.0.0.1:3",
        "MANYFORGE_HEADLESS_API_URL": "http://127.0.0.1:1",
    }


def test_named_built_frontend_is_served_by_composer_without_vite(
    monkeypatch: Any, tmp_path: Path
) -> None:
    # The prebuilt manyforge image: no operator package, the built GUI on
    # Composer's port and no Vite, npm or frontend port.
    static = tmp_path / "dist"
    static.mkdir()
    (static / "index.html").write_text("built fixture frontend")
    workbench, _ = _no_project_ros_workbench(
        monkeypatch, tmp_path, composer_static_dir=static, composer_bind_host="0.0.0.0")
    monkeypatch.setattr(dev_workbench, "_ensure_frontend_dependencies",
                        lambda: pytest.fail("a built frontend needs no npm dependencies"))
    checked_ports: list[int] = []
    monkeypatch.setattr(dev_workbench, "_port_available",
                        lambda port: checked_ports.append(port) or True)
    commands, _ = _prepared_start(workbench, monkeypatch)

    assert workbench._vite_command is None
    assert checked_ports == [1, 2, 3]
    composer = commands["Composer backend"]
    assert composer[composer.index("--static-dir") + 1] == str(static)
    assert composer[composer.index("--host") + 1] == "0.0.0.0"


def test_named_built_frontend_must_hold_an_index(monkeypatch: Any, tmp_path: Path) -> None:
    workbench, _ = _no_project_ros_workbench(
        monkeypatch, tmp_path, composer_static_dir=tmp_path / "missing")
    with pytest.raises(dev_workbench.WorkbenchError, match="built Composer frontend"):
        workbench.start()


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
    def unexpected_check() -> None:
        pytest.fail("invalid selection must fail before native artifact checks")

    monkeypatch.setattr(dev_workbench, "_current_backend", unexpected_check)
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
    assert workbench.manager is None


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
            "/fixture", allowed_roots=[REPOSITORY_ROOT / "examples"])


def test_ros_bootstrap_captures_io_provider_scenario(monkeypatch: Any) -> None:
    # As in the container: the workbench folder is this checkout.
    monkeypatch.setattr(dev_workbench, "WORKSPACE", REPOSITORY_ROOT)
    captured, source = dev_workbench._capture_ros_bootstrap_project(
        str(REPOSITORY_ROOT / "examples/real_lite6_pneumatic_pick_and_place.scenario"),
        allowed_roots=[REPOSITORY_ROOT / "examples"],
    )
    assert source.endswith("/examples/real_lite6_pneumatic_pick_and_place.scenario")
    # Machine and member components carry no runtime kind.
    assert sorted(
        component["configuration"]["runtime_kind"]
        for component in captured["deployment"]["components"]
        if "runtime_kind" in component.get("configuration", {})
    ) == ["io_provider", "ros2_control", "ros2_control"]


def test_ros_bootstrap_captures_authored_program_and_uses_production_preparer(
    monkeypatch: Any, tmp_path: Path
) -> None:
    from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
    from manyforge_headless_api import activation_preparer

    # As in the container: the workbench folder is this checkout, so the
    # example's model sources (manyforge_example_models) resolve.
    monkeypatch.setattr(dev_workbench, "WORKSPACE", REPOSITORY_ROOT)
    captured, source = dev_workbench._capture_ros_bootstrap_project(
        str(REPOSITORY_ROOT / "examples/sim_ur10e_pick_and_place.scenario"),
        allowed_roots=[REPOSITORY_ROOT / "examples"],
    )
    assert source.endswith("/examples/sim_ur10e_pick_and_place.scenario")
    assert captured["program"]["api_version"] == "manyforge.program.v4"
    assert captured["model_source_closure"]["components"]
    # The Program's scene meshes resolve against the Scenario project folder.
    meshes = captured["scene_mesh_closure"]["meshes"]
    assert captured["scene_mesh_closure"]["schema_version"] == (
        "manyforge.activation_scene_mesh_closure.v1")
    assert [row["resource_uri"] for row in meshes] == sorted(
        f"../isaac_manipulator_scene_light/meshes/{name}" for name in (
            "mac_n_cheese_centered_collider_cooked.obj",
            "World__small_KLT_collider_primitives.obj",
            "World__small_KLT_01_collider_primitives.obj",
            "World__soup_can_collider_cooked.obj",
            "background__table_low_327_collider_cooked.obj",
        ))
    assert sorted(
        component["configuration"]["runtime_kind"]
        for component in captured["deployment"]["components"]
    ) == ["pose_source", "ros2_control", "ros2_control"]
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
    _built_native_fixture(monkeypatch, extension=tmp_path / "extension", preparer=tmp_path / "preparer")
    monkeypatch.setattr(dev_workbench, "_ensure_ros_bootstrap_package", lambda *_a, **_k: tmp_path)
    workbench = dev_workbench.Workbench(
        project_path=None, bootstrap_project_path="/project", ros2_control=True,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    with pytest.raises(dev_workbench.WorkbenchError, match="changed during startup preparation"):
        workbench.start()
    assert workbench.manager is None


def test_runtime_capability_selection_keeps_one_build_closure(
    monkeypatch: Any, tmp_path: Path
) -> None:
    ros = (tmp_path / "worker", tmp_path / "ros")
    calls: list[str] = []
    monkeypatch.setattr(
        dev_workbench, "_current_ros_backend",
        lambda _environment: calls.append("ros") or ros,
    )
    monkeypatch.setattr(dev_workbench, "_ensure_current_ros_backend",
                        lambda: pytest.fail("serve checks the ROS product; it never builds it"))
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
        assert env["MANYFORGE_ROS2_CONTROL_BUILD_KEEP_GOING"] == "ON"
        ros_build.mkdir(parents=True, exist_ok=True)
        targets = ("manyforge_ros2_control_execution_host", "manyforge_roboplan_candidate_worker")
        for name in targets:
            source = ros_build / name
            source.write_bytes(name.encode())
            source.chmod(0o755)
        overlay.parent.mkdir(parents=True, exist_ok=True)
        overlay.write_text("overlay", encoding="utf-8")

    def source_overlay(command: tuple[str, ...], **kwargs: Any) -> Any:
        if command[-1] == "import pybind11; print(pybind11.get_cmake_dir())":
            assert command[0] == dev_workbench.sys.executable
            return SimpleNamespace(returncode=0, stdout="/pybind11/cmake\n")
        assert command[-1] == str(overlay)
        assert kwargs["env"]["ROS_STATIC_PEERS"] == "orin;laptop"
        return SimpleNamespace(
            returncode=0, stdout=b"AMENT_PREFIX_PATH=/current/overlay\0PYTHONPATH=/current/messages\0"
        )

    monkeypatch.setattr(dev_workbench, "_run", build)
    monkeypatch.setattr(dev_workbench.subprocess, "run", source_overlay)
    targets = dev_workbench._ensure_current_ros_backend()
    # One graph builds the whole native closure, the Python modules against
    # the serving interpreter's pybind11.
    assert commands == [(
        str(REPOSITORY_ROOT / "scripts/ros/build-ros2-control-runtime.sh"),
        "--source-dir", str(REPOSITORY_ROOT / "scripts/ros/ros-integration"),
        "--target", "manyforge_ros2_control_execution_host",
        "--target", "manyforge_roboplan_candidate_worker",
        "--target", "manyforge_manager_daemon",
        "--target", "manyforge_headless_py",
        "--target", "manyforge_core_py",
        "--cmake-arg", "-DMANYFORGE_BUILD_PYTHON_MODULES=ON",
        "--cmake-arg", "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -g",
        "--cmake-arg", "-Dpybind11_DIR=/pybind11/cmake",
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
    workbench._spawn_manager(("manager",), environment={})
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
        str(project), allowed_roots=[tmp_path]
    ) == "My_Cell-2"


def test_project_status_refuses_legacy_v2_project(tmp_path: Path) -> None:
    project = tmp_path / "legacy.scenario"
    project.mkdir()
    (project / "scenario.yaml").write_text(
        '{"api_version":"manyforge.scenario.project.v2",'
        '"scenario_id":"legacy","name":"legacy"}', encoding="utf-8"
    )
    with pytest.raises(dev_workbench.WorkbenchError, match="re-author.*direct references"):
        dev_workbench._project_scenario_id(str(project), allowed_roots=[tmp_path])


def test_project_status_accepts_examples_and_mounted_project_folders(
    monkeypatch: Any, tmp_path: Path
) -> None:
    examples, mounted, other = (tmp_path / name for name in ("examples", "mounted", "other"))
    for root in (examples, mounted, other):
        project = root / "cell.scenario"
        project.mkdir(parents=True)
        (project / "scenario.yaml").write_text(
            '{"api_version":"manyforge.scenario.project.v3","scenario_id":"cell","name":"cell"}',
            encoding="utf-8")
    monkeypatch.setenv("MANYFORGE_DEV_PROJECT_ROOTS", str(mounted))
    roots = dev_workbench._project_roots(examples)
    assert roots == [examples, mounted]
    for root in (examples, mounted):
        assert dev_workbench._project_scenario_id(str(root / "cell.scenario"), allowed_roots=roots) == "cell"
    with pytest.raises(dev_workbench.WorkbenchError, match="outside the workbench project root"):
        dev_workbench._project_scenario_id(str(other / "cell.scenario"), allowed_roots=roots)


def test_host_project_paths_map_onto_their_mounted_folder(monkeypatch: Any) -> None:
    monkeypatch.setenv("MANYFORGE_PROJECT_ROOT_MAPPINGS", json.dumps([
        {"hostPath": "/home/user/Projects", "containerPath": "/projects/Projects"},
        {"hostPath": "/data/Projects", "containerPath": "/projects/Projects-2"},
    ]))
    mapped = dev_workbench._container_project_path
    assert mapped("/home/user/Projects/cell.scenario") == "/projects/Projects/cell.scenario"
    assert mapped("/data/Projects/a/cell.scenario/") == "/projects/Projects-2/a/cell.scenario"
    # Container paths and paths outside every folder stay as given; the
    # project-root check refuses the latter.
    for unchanged in ("examples/cell.scenario", "/projects/Projects/cell.scenario",
                      "/home/user/Projects-old/cell.scenario"):
        assert mapped(unchanged) == unchanged
    monkeypatch.delenv("MANYFORGE_PROJECT_ROOT_MAPPINGS")
    assert mapped("/home/user/Projects/cell.scenario") == "/home/user/Projects/cell.scenario"


def test_composer_roots_include_launcher_mounted_folders(
    monkeypatch: Any, tmp_path: Path
) -> None:
    examples = tmp_path / "examples"
    monkeypatch.setenv(
        "MANYFORGE_DEV_PROJECT_ROOTS",
        "/projects/first,/projects/second",
    )
    # The examples stay the first (default browse) folder; ManyForge's own
    # content root comes last.
    assert dev_workbench._read_filesystem_roots(examples) == (
        examples, Path("/projects/first"), Path("/projects/second"), dev_workbench.WORKSPACE,
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
        dev_workbench._project_scenario_id(str(project), allowed_roots=[tmp_path])


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


class _FakeManager:
    """A Manager process that exits with ``status`` after ``timeouts`` waits.

    ``on_timeout`` runs at each timed-out wait (a stop request arriving).
    """

    def __init__(self, observed: list[str], status: int = 0, timeouts: int = 0,
                 on_timeout: Any = None) -> None:
        self.observed = observed
        self.status = status
        self.timeouts = timeouts
        self.on_timeout = on_timeout
        self.returncode: int | None = None

    def poll(self) -> int | None:
        return self.returncode

    def wait(self, timeout: float | None = None) -> int:
        self.observed.append(f"wait {timeout}")
        if self.timeouts:
            self.timeouts -= 1
            if self.on_timeout is not None:
                self.on_timeout()
            raise dev_workbench.subprocess.TimeoutExpired("manager", timeout)
        self.returncode = self.status
        return self.status

    def send_signal(self, number: int) -> None:
        self.observed.append(f"signal {int(number)}")

    def terminate(self) -> None:
        self.observed.append("terminate")

    def kill(self) -> None:
        self.observed.append("kill")


def test_close_asks_the_manager_to_end_the_application_then_waits(monkeypatch: Any) -> None:
    workbench = dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    observed: list[str] = []
    workbench.manager = _FakeManager(observed)
    workbench.stop_requested = True
    workbench.stop_requests = 1
    vite = SimpleNamespace(returncode=None)
    vite.poll = lambda: vite.returncode

    def vite_terminate() -> None:
        observed.append("vite terminate")
        vite.returncode = 0

    vite.terminate = vite_terminate
    vite.wait = lambda timeout: vite.returncode
    workbench.vite = vite

    def shutdown(path: Path, *, timeout_seconds: float) -> dict[str, Any]:
        observed.append(f"shutdown_runtime {path.name}")
        return {"ok": True}

    monkeypatch.setattr(dev_workbench, "request_manager_shutdown", shutdown)
    stopped: list[bool] = []
    workbench.session = SimpleNamespace(stopped=lambda *, verified: stopped.append(verified),
                                        close=lambda: None)

    workbench.close()

    # The Manager halts the robots, stops Composer, the Program runner and
    # Headless, then the host, and exits (spec 3.3 point 5); the launcher only
    # asks and waits, and stops Vite only afterwards (OD-187).
    assert observed == [
        "shutdown_runtime manager-command.sock",
        f"wait {dev_workbench.MANAGER_STOP_POLL_SECONDS}",
        "vite terminate",
    ]
    assert stopped == [True]


def test_close_waits_without_limit_and_passes_a_second_stop_request_on(monkeypatch: Any) -> None:
    # OD-187: no time limit and no kill here; a second SIGTERM/SIGINT to the
    # workbench is passed on to the Manager, which forces its stop and ends
    # with its "stop incomplete" status: not a clean stop.
    workbench = dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    workbench.stop_requested = True
    workbench.stop_requests = 1
    observed: list[str] = []
    timeouts: list[int] = []

    def second_request() -> None:
        timeouts.append(1)
        if len(timeouts) == 2:
            workbench.stop_requests += 1

    workbench.manager = _FakeManager(observed, status=74, timeouts=4, on_timeout=second_request)
    monkeypatch.setattr(dev_workbench, "request_manager_shutdown",
                        lambda path, *, timeout_seconds: observed.append("shutdown_runtime"))

    assert workbench._stop_manager() is False
    poll = f"wait {dev_workbench.MANAGER_STOP_POLL_SECONDS}"
    assert observed == ["shutdown_runtime", poll, poll, f"signal {int(dev_workbench.signal.SIGTERM)}",
                        poll, poll, poll]
    assert "terminate" not in observed and "kill" not in observed


def test_vite_starts_after_composer_answers_and_its_exit_ends_the_session(
    tmp_path: Path, monkeypatch: Any
) -> None:
    # Vite is not an application process (contract 1.1): this launcher starts
    # it once Composer answers, keeps it as its own child, and a Vite exit
    # ends the session by stopping the Manager, as before.
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.delenv("MANYFORGE_DEV_COLD_SESSION_ID", raising=False)
    workbench = dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    observed: list[str] = []
    composer_ready = iter([False, True])

    class Client:
        def __init__(self, *_args, **_kwargs):
            pass
        def __enter__(self):
            return self
        def __exit__(self, *_args):
            pass
        def health(self):
            return next(composer_ready)

    vite = SimpleNamespace(returncode=None)
    vite.poll = lambda: vite.returncode
    vite.terminate = lambda: observed.append("vite terminate")

    def popen(command, **kwargs):
        observed.append(f"vite start {kwargs['cwd'] == dev_workbench.FRONTEND_ROOT}")
        assert kwargs["env"]["MANYFORGE_COMPOSER_PROXY_TARGET"] == "http://127.0.0.1:3"
        assert callable(kwargs["preexec_fn"])
        return vite

    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", Client)
    monkeypatch.setattr(dev_workbench.subprocess, "Popen", popen)
    monkeypatch.setattr(dev_workbench, "request_manager_shutdown",
                        lambda path, *, timeout_seconds: observed.append("shutdown_runtime"))
    monkeypatch.setattr(dev_workbench, "Session", SimpleNamespace(
        direct=lambda _root, _disk_limits: SimpleNamespace(
            stopped=lambda *, verified: observed.append(f"stopped {verified}"),
            close=lambda: None)))

    def start() -> None:
        workbench._vite_command = ("/usr/bin/node", "vite.js")
        workbench.manager = _FakeManager(observed, status=0, timeouts=3)

    def report() -> None:
        if workbench.vite is not None and observed.count("report") == 1:
            vite.returncode = 1
        observed.append("report")

    monkeypatch.setattr(workbench, "start", start)
    monkeypatch.setattr(workbench, "_report_ready", report)

    with pytest.raises(dev_workbench.WorkbenchError, match="Vite HMR exited with status 1"):
        workbench.serve()

    assert observed == [
        "wait 0.5", "report",                      # Composer not ready: no Vite yet
        "wait 0.5", "vite start True", "report",   # Composer ready: Vite started once
        "wait 0.5", "shutdown_runtime",            # Vite exited: the Manager is stopped
        f"wait {dev_workbench.MANAGER_STOP_POLL_SECONDS}", "stopped False",
    ]


def test_serve_waits_on_the_manager_and_reports_its_exit(tmp_path: Path, monkeypatch: Any) -> None:
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.delenv("MANYFORGE_DEV_COLD_SESSION_ID", raising=False)
    workbench = dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    observed: list[str] = []
    monkeypatch.setattr(dev_workbench, "Session", SimpleNamespace(
        direct=lambda _root, _disk_limits: SimpleNamespace(
            stopped=lambda *, verified: observed.append(f"stopped {verified}"),
            close=lambda: None)))

    def start() -> None:
        workbench.manager = _FakeManager(observed, status=72, timeouts=1)

    monkeypatch.setattr(workbench, "start", start)
    monkeypatch.setattr(workbench, "_report_ready", lambda: observed.append("report"))

    with pytest.raises(dev_workbench.WorkbenchError, match="status 72"):
        workbench.serve()

    # Reporting only between waits; the exit is the Manager's (it printed
    # which process exited), and the session closes unverified.
    assert observed == ["wait 0.5", "report", "wait 0.5", "stopped False"]


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
    site.write_text('{"schema":"manyforge.site.v2","device_addresses":{}}')
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


@pytest.mark.parametrize("current,scenario_ready,expected", [
    ("cell", True, True), ("cell", False, False), ("other", True, False)])
def test_operator_status_with_a_scenario_requires_it_current_and_ready(
    tmp_path, monkeypatch, current, scenario_ready, expected
):
    # The prebuilt image's `run --project`: every owner ready, and the named
    # Scenario is the current, ready one.
    class Client:
        def __init__(self, url, **kwargs):
            pass
        def __enter__(self):
            return self
        def __exit__(self, *args):
            pass
        def manager_status(self):
            return {"source": {"available": True, "freshness": "fresh", "known_gaps": []},
                    "status": {"manager_ready": True}}
        def health(self):
            return {"status": "ready", "headless_projection_ready": True}
        def request(self, method, path):
            if path == "/api/scenarios/current":
                return {"ready": scenario_ready, "current_scenario": {"scenario_id": current}}
            return {"started": True, "closed": False}
    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", Client)
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path)
    status = dev_workbench.query_operator_status(
        headless_port=9010, runner_port=9011, composer_port=9000, scenario_id="cell")
    assert status["ready"] is expected
    assert status["scenario"]["current_scenario"] == (
        {"scenario_id": current} if current == "cell" else None)
    assert dev_workbench.query_operator_status(
        headless_port=9010, runner_port=9011, composer_port=9000)["ready"] is True


def test_g_package_mismatch_refuses_before_artifact_checks_or_process_start(tmp_path, monkeypatch):
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
    monkeypatch.setattr(dev_workbench, "_current_backend", lambda: pytest.fail("must refuse changed package first"))
    with pytest.raises(dev_workbench.WorkbenchError, match="aggregate mismatch"):
        workbench.start()
    assert workbench.manager is None


def test_g_refuses_a_missing_built_frontend_before_process_start(tmp_path, monkeypatch):
    from manyforge_operator.package_aggregate import package_aggregate_sha256
    install = tmp_path / "package"
    install.mkdir()
    (install / "empty.json").write_text("{}")
    monkeypatch.setattr(dev_workbench, "FRONTEND_DIST", tmp_path / "dist")
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=True,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
        operator_install_root=install, operator_cell_id="fixture_cell",
        canonical_empty_admission_filename="empty.json",
        package_aggregate_digest=package_aggregate_sha256(install))
    monkeypatch.setattr(dev_workbench, "_current_backend", lambda: pytest.fail("must refuse first"))
    with pytest.raises(dev_workbench.WorkbenchError, match="built Composer frontend"):
        workbench.start()
    assert workbench.manager is None


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
    # Recorded by the separate build step; arbitrary fixture duration.
    native_timing = tmp_path / "native-build-timing.json"
    native_timing.write_text('{"build_elapsed_seconds":120.0}')
    monkeypatch.setattr(dev_workbench, "NATIVE_BUILD_TIMING", native_timing)
    monkeypatch.setattr(dev_workbench.time, "monotonic", lambda: clock[0])
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", manager_binary)
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    _built_native_fixture(monkeypatch, extension=extension, preparer=tmp_path / "preparer")
    monkeypatch.setattr(dev_workbench, "_port_available", lambda port: port != 3000)
    built_frontend = tmp_path / "dist"
    built_frontend.mkdir()
    (built_frontend / "index.html").write_text("built fixture frontend")
    monkeypatch.setattr(dev_workbench, "FRONTEND_DIST", built_frontend)
    monkeypatch.setattr(dev_workbench, "_ensure_frontend_dependencies", lambda: pytest.fail("G must not npm ci"))
    monkeypatch.setattr(dev_workbench, "_ensure_repository_bootstrap_package", lambda: pytest.fail("G must use its qualified package"))
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=True,
        headless_port=9010, runner_port=9011, composer_port=9000, frontend_port=3000,
        operator_install_root=install, operator_cell_id="fixture_cell",
        operator_instance_id="fixture-instance", canonical_empty_admission_filename="empty.json",
        package_aggregate_digest=package_aggregate_sha256(install),
        composer_bind_host="0.0.0.0")
    monkeypatch.setattr(workbench, "_simulation_control_arguments", lambda: ())
    launched: list[tuple[tuple[str, ...], dict[str, str]]] = []

    def spawn_manager(command, *, environment):
        logs = tmp_path / "state/current/execution-run/ros" / f"1-{session_started_ns}"
        assert os.environ["ROS_LOG_DIR"] == str(logs)
        assert logs.is_dir()
        assert os.environ["ROS_HOME"] == str(tmp_path / "state/current/execution-run/ros-home")
        launched.append((tuple(command), environment))

    monkeypatch.setattr(workbench, "_spawn_manager", spawn_manager)

    class Client:
        def __init__(self, *args, **kwargs):
            pass
        def __enter__(self):
            return self
        def __exit__(self, *args):
            pass
        def request(self, *args):
            return {"started": True, "closed": False}
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
            return {"status": "ready", "headless_projection_ready": True}
    monkeypatch.setattr(dev_workbench, "HeadlessHttpClient", Client)

    workbench.start()

    # The launcher starts only the Manager and hands it the process list.
    assert len(launched) == 1
    manager_command, manager_environment = launched[0]
    document = json.loads(Path(
        manager_command[manager_command.index("--application-processes") + 1]).read_text())
    assert [(row["role"], row["start"]) for row in document["processes"]] == [
        ("headless", "immediate"), ("program_runner", "immediate"), ("composer", "after_baseline"),
    ]
    assert workbench._vite_command is None  # No Vite in operator mode.
    commands = {row["role"]: row["argv"] for row in document["processes"]}
    assert "--assistant-bridge-url" not in commands["composer"]
    assert not any("manyforge_assistant_bridge" in part
                   for command in (manager_command, *commands.values()) for part in command)
    # The execution host and its component processes (a point-cloud source,
    # the assistant's host and bridge) inherit both.
    assert manager_environment == {
        "MANYFORGE_COMPOSER_URL": "http://127.0.0.1:9000",
        "MANYFORGE_HEADLESS_API_URL": "http://127.0.0.1:9010",
    }
    assert os.environ["ROS_LOG_DIR"] == str(tmp_path / "state/current/execution-run/ros" / f"1-{session_started_ns}")
    assert not (tmp_path / "foreign-home").exists()
    assert not (tmp_path / "foreign-logs").exists()
    for name in ("program_runner", "headless"):
        command = commands[name]
        assert command[command.index("--cell-id") + 1] == "fixture_cell"
        assert "--owner-id" not in command
    assert manager_command[manager_command.index("--install-root") + 1] == str(install)
    assert manager_command[manager_command.index("--canonical-empty-admission-filename") + 1] == "empty.json"

    # Readiness is reported, never awaited with a deadline (spec 3.3).
    workbench._report_ready()
    timing = json.loads((tmp_path / "state/current/workbench-startup-timing.json").read_text())
    assert timing == {"build_elapsed_seconds": 120.0, "readiness_elapsed_seconds": 0.0}
    assert workbench._ready_reported is True


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
        assert not (root / "previous-runs").exists()
        started.append(marker["session_id"])
        workbench.stop_requested = True

    monkeypatch.setattr(workbench, "start", start)
    monkeypatch.setattr(workbench, "_stop_manager", lambda: True)
    workbench.serve()
    assert len(started) == 1
    assert workbench.session is None
    marker = json.loads((root / "current/session.json").read_text())
    assert marker["session_id"] == started[0] and marker["disposition"] == "stopped"


def test_preselected_session_report_is_passed_to_headless(tmp_path, monkeypatch):
    root = tmp_path / "state"
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", root)
    workbench = dev_workbench.Workbench(project_path=None, ros2_control=False,
        headless_port=1, runner_port=2, composer_port=3, frontend_port=4)
    assert workbench._previous_session_arguments() == ()
    previous = "a" * 32
    workbench.session = SimpleNamespace(marker={"previous_session": {"session_id": previous}})
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
    monkeypatch.setattr(dev_workbench, "_pybind11_cmake_dir", lambda: "/pybind11/cmake")
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


def test_build_command_runs_the_native_steps_in_order_and_records_build_time(
    tmp_path: Path, monkeypatch: Any
) -> None:
    calls: list[str] = []
    for build in ("_ensure_current_ros_backend", "_current_backend",
                  "_current_core_bindings", "_ensure_activation_preparer"):
        monkeypatch.setattr(dev_workbench, build, lambda _build=build: calls.append(_build))
    monkeypatch.setattr(dev_workbench, "_run", lambda command: calls.append(tuple(command)))
    clock = iter((10.0, 52.5))
    monkeypatch.setattr(dev_workbench.time, "monotonic", lambda: next(clock))
    timing = tmp_path / "native-build-timing.json"
    monkeypatch.setattr(dev_workbench, "NATIVE_BUILD_TIMING", timing)

    assert dev_workbench.main(["build"]) == 0

    # The one graph builds first; its Manager and modules are checked before
    # the separately linked authoring tools build.
    assert calls[:4] == ["_ensure_current_ros_backend", "_current_backend",
                         "_current_core_bindings", "_ensure_activation_preparer"]
    # Then the served Python packages are compiled by the services' interpreter.
    compile_command = calls[4]
    assert compile_command[:3] == (dev_workbench.sys.executable, "-m", "compileall")
    assert str(dev_workbench.WORKSPACE / "manyforge_headless_api") in compile_command
    assert json.loads(timing.read_text()) == {"build_elapsed_seconds": 42.5}
    monkeypatch.setattr(dev_workbench, "NATIVE_BUILD_TIMING", tmp_path / "absent.json")
    assert dev_workbench._native_build_elapsed() is None


def test_build_command_builds_the_frontend_only_when_asked(
    tmp_path: Path, monkeypatch: Any
) -> None:
    calls: list[str] = []
    for build in ("_ensure_current_ros_backend", "_current_backend",
                  "_current_core_bindings", "_ensure_activation_preparer"):
        monkeypatch.setattr(dev_workbench, build, lambda: None)
    monkeypatch.setattr(dev_workbench, "_run", lambda command: None)
    monkeypatch.setattr(dev_workbench, "_ensure_frontend_build", lambda: calls.append("frontend"))
    monkeypatch.setattr(dev_workbench, "NATIVE_BUILD_TIMING", tmp_path / "timing.json")

    assert dev_workbench.main(["build"]) == 0
    assert calls == []
    assert dev_workbench.main(["build", "--frontend"]) == 0
    assert calls == ["frontend"]


def test_frontend_build_is_skipped_while_dist_matches_the_source(
    tmp_path: Path, monkeypatch: Any
) -> None:
    dist = tmp_path / "frontend/dist"
    marker = dist / ".manyforge-source.sha256"
    monkeypatch.setattr(dev_workbench, "FRONTEND_DIST", dist)
    monkeypatch.setattr(dev_workbench, "FRONTEND_DIST_SOURCE_HASH", marker)
    monkeypatch.setattr(dev_workbench, "_ensure_frontend_dependencies", lambda: None)
    source = ["a" * 64]
    monkeypatch.setattr(dev_workbench, "_frontend_source_hash", lambda: source[0])
    builds: list[tuple[str, ...]] = []

    def npm(command: Any, *, cwd: Path) -> None:
        # Vite empties dist before it writes the new build.
        assert cwd == dev_workbench.FRONTEND_ROOT
        builds.append(tuple(command))
        if dist.exists():
            for child in dist.iterdir():
                child.unlink()
        dist.mkdir(parents=True, exist_ok=True)
        (dist / "index.html").write_text("built")

    monkeypatch.setattr(dev_workbench, "_run", npm)

    dev_workbench._ensure_frontend_build()
    assert builds == [("npm", "run", "build")]
    assert marker.read_text().strip() == "a" * 64
    dev_workbench._ensure_frontend_build()
    assert len(builds) == 1
    source[0] = "b" * 64
    dev_workbench._ensure_frontend_build()
    assert len(builds) == 2 and marker.read_text().strip() == "b" * 64
    (dist / "index.html").unlink()
    dev_workbench._ensure_frontend_build()
    assert len(builds) == 3


def test_frontend_source_hash_ignores_build_outputs(tmp_path: Path, monkeypatch: Any) -> None:
    frontend = tmp_path / "frontend"
    (frontend / "src").mkdir(parents=True)
    (frontend / "src/App.tsx").write_text("export const value = 1;\n")
    monkeypatch.setattr(dev_workbench, "WORKSPACE", REPOSITORY_ROOT)
    monkeypatch.setattr(dev_workbench, "FRONTEND_ROOT", frontend)
    base = dev_workbench._frontend_source_hash()
    for output in ("dist/index.html", "node_modules/x.js", ".tsbuildinfo/tsconfig.app.tsbuildinfo"):
        (frontend / output).parent.mkdir(parents=True, exist_ok=True)
        (frontend / output).write_text("generated\n")
    assert dev_workbench._frontend_source_hash() == base
    (frontend / "src/App.tsx").write_text("export const value = 2;\n")
    assert dev_workbench._frontend_source_hash() != base


def test_build_command_failure_exits_unavailable(monkeypatch: Any, capsys: Any) -> None:
    def failed_build() -> None:
        raise dev_workbench.WorkbenchError("command failed with exit 1: cmake --build")

    monkeypatch.setattr(dev_workbench, "_ensure_current_ros_backend", failed_build)
    assert dev_workbench.main(["build"]) == 69
    assert "cmake --build" in capsys.readouterr().err


@pytest.mark.parametrize("missing", [
    "manager", "headless", "ros_worker", "ros_host", "ros_overlay",
    "preparer", "generator", "core",
])
def test_serve_refuses_missing_native_artifacts_with_the_build_step(
    tmp_path: Path, monkeypatch: Any, missing: str
) -> None:
    build = tmp_path / "build"
    artifacts = {
        "manager": (build / "ros-integration/headless/manyforge_manager_daemon", True),
        "headless": (build / "ros-integration/headless/manyforge_headless_py.fixture.so", False),
        "ros_worker": (build / "ros-integration/manyforge_roboplan_candidate_worker", True),
        "ros_host": (build / "ros-integration/manyforge_ros2_control_execution_host", True),
        "ros_overlay": (build / "isaac/ros-overlay/install/setup.bash", False),
        "preparer": (build / "deployment-authoring/manyforge_prepare_activation", True),
        "generator": (build / "deployment-authoring/manyforge_generate_canonical_empty_package", True),
        "core": (build / "ros-integration/core-python/manyforge_core_py.fixture.so", False),
    }
    for name, (path, executable) in artifacts.items():
        if name == missing:
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("true\n")
        if executable:
            path.chmod(0o755)
    monkeypatch.setattr(dev_workbench.os, "environ", dict(os.environ))
    monkeypatch.delenv("MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS", raising=False)
    monkeypatch.setenv("MANYFORGE_DEV_BUILD_JOBS", "1")
    monkeypatch.setattr(dev_workbench, "BUILD_ROOT", build)
    monkeypatch.setattr(dev_workbench, "HEADLESS_BUILD", build / "ros-integration/headless")
    monkeypatch.setattr(dev_workbench, "MANAGER_BINARY", artifacts["manager"][0])
    monkeypatch.setattr(dev_workbench, "ACTIVATION_PREPARER", artifacts["preparer"][0])
    monkeypatch.setattr(dev_workbench, "CANONICAL_EMPTY_GENERATOR", artifacts["generator"][0])
    monkeypatch.setattr(dev_workbench, "CORE_PYTHON_BUILD", build / "ros-integration/core-python")
    monkeypatch.setattr(dev_workbench, "NATIVE_BUILD_TIMING", build / "native-build-timing.json")
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.setattr(dev_workbench, "_port_available", lambda _port: True)
    monkeypatch.setattr(dev_workbench, "_run", lambda *_a, **_k: pytest.fail("serve must not build"))
    workbench = dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    )
    with pytest.raises(dev_workbench.NativeBuildMissingError,
                       match="run the native build step first: python3 scripts/dev-workbench.py build"):
        workbench.start()
    assert workbench.manager is None


@pytest.mark.parametrize("variable, member", [
    ("MANYFORGE_LOG_EVIDENCE_MAX_DISK_PERCENT", "disk_max_percent"),
    ("MANYFORGE_LOG_EVIDENCE_MIN_FREE_BYTES", "disk_always_free_bytes"),
])
def test_serve_refuses_the_removed_disk_variables(
    tmp_path: Path, monkeypatch: Any, variable: str, member: str
) -> None:
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    monkeypatch.setenv(variable, "10")
    with pytest.raises(dev_workbench.WorkbenchError, match=(
            f"{variable} is no longer read: set {member} in the site settings file")):
        dev_workbench.Workbench(
            project_path=None, headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
        )


def test_serve_takes_the_disk_limits_from_the_site_file(tmp_path: Path, monkeypatch: Any) -> None:
    monkeypatch.setattr(dev_workbench, "STATE_ROOT", tmp_path / "state")
    for variable in dev_workbench.REMOVED_DISK_VARIABLES:
        monkeypatch.delenv(variable, raising=False)
    site = tmp_path / "manyforge.site.json"
    site.write_text('{"schema":"manyforge.site.v2","disk_machine_minimum_bytes":2000000000}')
    workbench = dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
        site_settings=site,
    )
    assert workbench.disk_limits.floor_bytes == 12_000_000_000
    assert dev_workbench.Workbench(
        project_path=None, headless_port=1, runner_port=2, composer_port=3, frontend_port=4,
    ).disk_limits == dev_workbench.run_session.DiskLimits()
