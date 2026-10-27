#!/usr/bin/env python3
"""D0 development workbench orchestration inside one dev container."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
from typing import Any, Callable, Sequence

# `docker exec` does not inherit shell exports made by the container
# entrypoint. Keep this tool and every service it starts self-contained.
_SOURCE_ROOTS = (
    "/workspace",
    "/workspace/manyforge_behavior",
    "/workspace/manyforge_ros",
)
for _source_root in _SOURCE_ROOTS:
    if _source_root not in sys.path:
        sys.path.insert(0, _source_root)
os.environ["PYTHONPATH"] = os.pathsep.join(
    (*_SOURCE_ROOTS, os.environ.get("PYTHONPATH", ""))
).rstrip(os.pathsep)

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from scripts.lib.export_retention import create_host_ros_log_session
from scripts.lib.workbench_session import Session


WORKSPACE = Path("/workspace")
CACHE_ROOT = Path("/runner")
BUILD_ROOT = CACHE_ROOT / "build"
STATE_ROOT = Path(os.environ.get("MANYFORGE_DEV_STATE_ROOT", "/tmp/manyforge-state"))
ASSISTANT_BRIDGE_PORT = 8100
HEADLESS_BUILD = BUILD_ROOT / "headless"
DEPLOYMENT_AUTHORING_BUILD = BUILD_ROOT / "deployment-authoring"
BOOTSTRAP_ROOT = CACHE_ROOT / "repository-bootstrap-package"
# Installation configuration, independent of the optional startup project.
# Preserve the existing workbench identity; never mint one per Load or restart.
WORKBENCH_CELL_ID = "c1_build_authority_cell"
BOOTSTRAP_ADMISSION = f"canonical-empty-admission.{WORKBENCH_CELL_ID}.json"
ACTIVATION_PREPARER = (
    DEPLOYMENT_AUTHORING_BUILD / "manyforge_prepare_activation"
)
CANONICAL_EMPTY_GENERATOR = (
    DEPLOYMENT_AUTHORING_BUILD / "manyforge_generate_canonical_empty_package"
)
MANAGER_BINARY = HEADLESS_BUILD / "manyforge_manager_daemon"
NODE_CATALOG = WORKSPACE / "manyforge_behavior/resources/node_catalog.yaml"
NODE_ENVIRONMENT_MANIFEST = (
    WORKSPACE / "manyforge_behavior/resources/node_environment_manifest.yaml"
)
ALARM_CATALOG = (
    WORKSPACE / "manyforge_headless/resources/operator-alarm-catalog.v1.yaml"
)
FRONTEND_ROOT = WORKSPACE / "manyforge_composer/frontend"


class WorkbenchError(RuntimeError):
    """One precise orchestration failure."""


def _project_scenario_id(raw_path: str, *, allowed_root: Path) -> str:
    """Read only the bounded project identity needed by launcher status."""

    from manyforge_behavior.canonical_artifact import (
        StrictArtifactError,
        load_strict_json_yaml,
    )

    root = allowed_root.resolve(strict=True)
    candidate = Path(raw_path).expanduser()
    project = candidate.resolve(strict=True)
    if (
        candidate.is_symlink()
        or not project.is_dir()
        or not project.is_relative_to(root)
    ):
        raise WorkbenchError("Scenario project is outside the workbench project root")
    manifest = project / "scenario.yaml"
    if (
        manifest.is_symlink()
        or not manifest.is_file()
        or manifest.stat().st_size > 64 * 1024
    ):
        raise WorkbenchError("Scenario project manifest is unavailable or oversized")
    try:
        value = load_strict_json_yaml(manifest)
    except StrictArtifactError as error:
        raise WorkbenchError(str(error)) from error
    scenario_id = value.get("scenario_id")
    if (
        value.get("api_version") != "manyforge.scenario.project.v2"
        or not isinstance(scenario_id, str)
        # One name: folder `<name>.scenario`, scenario_id == name == <name>.
        or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,127}", scenario_id) is None
        or value.get("name") != scenario_id
        or project.name != f"{scenario_id}.scenario"
    ):
        raise WorkbenchError(
            "Scenario project identity is invalid: the folder must be "
            "'<name>.scenario' and its scenario_id and name must both be <name>"
        )
    return scenario_id


def _json(value: Any) -> str:
    return json.dumps(value, allow_nan=False, indent=2, sort_keys=True)


def _composer_filesystem_roots(artifact_root: Path) -> str:
    """Keep examples available alongside folders mounted by the launcher."""

    extra_roots = os.environ.get("MANYFORGE_DEV_PROJECT_ROOTS", "")
    return f"{artifact_root},{extra_roots}" if extra_roots else str(artifact_root)


def _run(
    command: Sequence[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> None:
    print("+ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, env=env, check=False)
    if completed.returncode != 0:
        raise WorkbenchError(
            f"command failed with exit {completed.returncode}: {' '.join(command)}"
        )


def _persistent_key_register_path() -> Path:
    """Installation state beside the Composer settings, owned by the runner."""

    state_dir = os.environ.get("MANYFORGE_STATE_DIR", "").strip()
    if not state_dir or not Path(state_dir).is_absolute():
        raise WorkbenchError(
            "MANYFORGE_STATE_DIR must name the persistent installation state "
            "directory (scripts/compose-dev.sh sets it)"
        )
    return Path(state_dir) / "program-runner" / "persistent-keys.json"


def _build_jobs() -> str:
    jobs = os.environ.get("MANYFORGE_DEV_BUILD_JOBS", "").strip()
    if not jobs:
        # The native closure has several memory-heavy C++ translation units.
        # Eight keeps ordinary development hosts responsive without serializing
        # the build; callers may explicitly tune the shared limit.
        jobs = str(min(8, max(1, os.cpu_count() or 1)))
    if not jobs.isdecimal() or int(jobs) < 1:
        raise WorkbenchError("MANYFORGE_DEV_BUILD_JOBS must be a positive integer")
    return jobs


def _ensure_current_backend() -> Path:
    """Build only the stale current Manager/Headless production closure."""

    cache = HEADLESS_BUILD / "CMakeCache.txt"
    if not cache.is_file():
        completed = subprocess.run(
            (
                sys.executable,
                "-c",
                "import pybind11; print(pybind11.get_cmake_dir())",
            ),
            check=False,
            capture_output=True,
            text=True,
        )
        pybind11_dir = completed.stdout.strip()
        if completed.returncode != 0 or not pybind11_dir:
            raise WorkbenchError(
                "cannot resolve the development pybind11 CMake package"
            )
        _run(
            (
                "cmake",
                "-G",
                "Ninja",
                "-S",
                str(WORKSPACE / "manyforge_headless"),
                "-B",
                str(HEADLESS_BUILD),
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                "-DBUILD_TESTING=OFF",
                "-DMANYFORGE_HEADLESS_BUILD_PYTHON=ON",
                f"-Dpybind11_DIR={pybind11_dir}",
            )
        )
    _run(
        (
            "cmake",
            "--build",
            str(HEADLESS_BUILD),
            "--parallel",
            _build_jobs(),
            "--target",
            "manyforge_manager_daemon",
            "manyforge_headless_py",
            "--",
            "-k",
            "0",
        )
    )
    _regular_file(MANAGER_BINARY, "current Manager", executable=True)
    extensions = sorted(HEADLESS_BUILD.glob("manyforge_headless_py*.so"))
    if len(extensions) != 1:
        raise WorkbenchError("current Headless Python extension is absent or ambiguous")
    _regular_file(extensions[0], "current Headless Python extension")
    return extensions[0]


def _ros_setup_environment(
    setups: Sequence[Path], environment: dict[str, str]
) -> dict[str, str]:
    """Import explicitly selected ROS setups in order, using literal paths."""

    completed = subprocess.run(
        ("bash", "-c", 'for setup in "$@"; do source "$setup" >/dev/null || exit; done; env -0',
         "bash", *(str(setup) for setup in setups)),
        check=False,
        capture_output=True,
        env=environment,
    )
    if completed.returncode != 0:
        raise WorkbenchError(
            "cannot source ROS setup scripts: " + ", ".join(str(setup) for setup in setups)
        )
    imported = {}
    for entry in completed.stdout.split(b"\0"):
        if entry:
            key, value = os.fsdecode(entry).split("=", 1)
            imported[key] = value
    return imported


def _ensure_current_ros_backend() -> tuple[Path, Path]:
    """Prepare the separately installed ROS product; never launch a provider."""

    build_root = BUILD_ROOT / "isaac"
    ros_build = BUILD_ROOT / "ros-integration"
    overlay = build_root / "ros-overlay/install/setup.bash"
    environment = os.environ.copy()
    configured_setups = environment.get("MANYFORGE_ROS_DEPENDENCY_SETUP_SCRIPTS", "")
    if configured_setups:
        setups = []
        for configured in configured_setups.split(os.pathsep):
            setup = Path(configured)
            if not configured or not setup.is_absolute() or not setup.is_file():
                raise WorkbenchError(
                    f"configured ROS dependency setup is unavailable: {configured!r}"
                )
            setups.append(setup)
        environment = _ros_setup_environment(setups, environment)
    environment.update(
        WORKSPACE=str(WORKSPACE),
        MANYFORGE_ROS2_CONTROL_BUILD_ROOT=str(build_root),
        MANYFORGE_ROBOPLAN_BUILD_DIR=str(ros_build),
        MANYFORGE_ISAAC_ROS_OVERLAY=str(build_root / "ros-overlay"),
        CMAKE_BUILD_PARALLEL_LEVEL=_build_jobs(),
        CMAKE_GENERATOR="Ninja",
        MANYFORGE_ROS2_CONTROL_BUILD_TESTING=os.environ.get(
            "MANYFORGE_ROS2_CONTROL_BUILD_TESTING", "OFF"
        ),
    )
    _run(
        (
            str(WORKSPACE / "scripts/ros/build-ros2-control-runtime.sh"),
            "--source-dir",
            str(WORKSPACE / "scripts/ros/ros-integration"),
        ),
        env=environment,
    )
    host = ros_build / "manyforge_ros2_control_execution_host"
    targets = (
        ros_build / "manyforge_roboplan_candidate_worker",
        host,
    )
    for target in targets:
        _regular_file(target, f"current {target.name}", executable=True)
    _regular_file(overlay, "current ROS overlay setup")
    # The build helper's exports belong to its subprocess. Import the generated
    # overlay before spawning the Runner or Manager; Manager's installed-target
    # children inherit this same environment through their existing protocol.
    os.environ.update(_ros_setup_environment((overlay,), environment))
    return targets


def _stage_runtime_targets(sources: Sequence[Path], installed: Path) -> None:
    for source in sources:
        target = installed / source.name
        shutil.copy2(source, target)
        _regular_file(target, f"installed {source.name}", executable=True)


def _ensure_activation_preparer() -> Path:
    """Build repository-owned activation and empty-bootstrap tools."""

    if not (DEPLOYMENT_AUTHORING_BUILD / "CMakeCache.txt").is_file():
        _run(
            (
                "cmake",
                "-G",
                "Ninja",
                "-S",
                str(WORKSPACE / "manyforge_deployment"),
                "-B",
                str(DEPLOYMENT_AUTHORING_BUILD),
                "-DCMAKE_BUILD_TYPE=Release",
                "-DBUILD_TESTING=OFF",
            )
        )
    _run(
        (
            "cmake",
            "--build",
            str(DEPLOYMENT_AUTHORING_BUILD),
            "--parallel",
            _build_jobs(),
            "--target",
            "manyforge_prepare_activation",
            "manyforge_generate_canonical_empty_package",
            "--",
            "-k",
            "0",
        )
    )
    _regular_file(ACTIVATION_PREPARER, "activation preparer", executable=True)
    _regular_file(
        CANONICAL_EMPTY_GENERATOR,
        "canonical-empty generator",
        executable=True,
    )
    return ACTIVATION_PREPARER


def _ensure_repository_bootstrap_package() -> Path:
    """Materialize selected infrastructure without selecting any Scenario."""

    runtime = "ros2_control"
    profile = (
        WORKSPACE / "manyforge_deployment/resources/maintained/catalog/"
        f"runtime_profile/manyforge.{runtime}_execution_runtime.json"
    )
    # Cache placement only, not a semantic artifact identity. Never overwrite
    # an immutable admission created by an older compiler/profile.
    digest = hashlib.sha256(f"{runtime}-workbench:{WORKBENCH_CELL_ID}\0".encode())
    for source in (CANONICAL_EMPTY_GENERATOR, profile):
        with source.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        digest.update(b"\0")
    package_root = BOOTSTRAP_ROOT / digest.hexdigest()
    package_root.mkdir(parents=True, exist_ok=True)
    _run(
        (
            str(CANONICAL_EMPTY_GENERATOR),
            "--output-root",
            str(package_root),
            "--cell-id",
            WORKBENCH_CELL_ID,
            "--ros-runtime-profile",
            str(profile),
        )
    )
    _regular_file(
        package_root / BOOTSTRAP_ADMISSION,
        "repository canonical-empty admission",
    )
    return package_root


def _capture_ros_bootstrap_project(
    project_path: str, *, allowed_root: Path
) -> tuple[dict[str, Any], str]:
    """Capture the intended first Load through the ordinary authoring owners."""

    from manyforge_behavior.model_source_capture import (
        ModelSourceCaptureError,
        capture_deployment_model_sources,
    )
    from manyforge_behavior.node_environment import build_node_environment
    from manyforge_composer.backend.authoring_forest import (
        AuthoringForestError,
        derive_candidate,
        validate_invariants,
    )
    from manyforge_composer.backend.scenario_artifacts import (
        ScenarioArtifactError,
        capture_scenario_artifact,
    )
    from manyforge_composer.backend.state import canonical_project_path

    try:
        scenario, source_path, source_digest = capture_scenario_artifact(
            project_path, allowed_roots=[str(allowed_root)]
        )
        deployment = scenario["deployment_draft"]
        runtime_kinds = {
            component.get("configuration", {}).get("runtime_kind")
            for component in (deployment or {}).get("components", [])
            if "runtime_kind" in component.get("configuration", {})
        }
        # Model-free pose sources run inside the same ROS execution host.
        if runtime_kinds - {"pose_source"} != {"ros2_control"}:
            raise WorkbenchError(
                "ROS startup requires an intended first Scenario with only "
                "ros2_control execution components"
            )
        catalog = build_node_environment().catalog
        authoring = scenario["program_draft"]
        if authoring is None or scenario["program_role_assignment"] is None:
            raise WorkbenchError("intended first ROS Scenario is incomplete")
        validate_invariants(authoring, catalog)
        program, diagnostics = derive_candidate(authoring, catalog)
        no_program = authoring.get("forest") == [] and not authoring.get("scene")
        if diagnostics or (program is None and not no_program):
            raise WorkbenchError(
                "intended first ROS Scenario has an incomplete authored Program"
            )
        captured = {
            "scenario_id": scenario["scenario_id"],
            "project_path": canonical_project_path(source_path),
            "source_document_digest": source_digest,
            "deployment": deployment,
            "model_source_closure": capture_deployment_model_sources(
                deployment, project_root=Path(source_path)
            ),
            "program": program.to_dict() if program is not None else None,
            "program_role_assignment": scenario["program_role_assignment"],
        }
    except (ScenarioArtifactError, AuthoringForestError, ModelSourceCaptureError) as error:
        raise WorkbenchError(str(error)) from error
    return captured, source_path


def _ensure_ros_bootstrap_package(
    captured: dict[str, Any], *, activation_preparer: Path
) -> Path:
    """Publish the exact first Deployment's empty predecessor before Manager."""

    from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
    from manyforge_headless_api.activation_preparer import ActivationPreparer
    from manyforge_headless_api.native import HeadlessApiError

    captured_digest = raw_sha256_digest(canonical_json_bytes(captured))
    digest = hashlib.sha256(
        f"ros-workbench:{WORKBENCH_CELL_ID}\0{captured_digest}\0".encode()
    )
    digest.update(activation_preparer.read_bytes())
    package_root = BOOTSTRAP_ROOT / digest.hexdigest()
    package_root.mkdir(parents=True, exist_ok=True)
    try:
        preparer = ActivationPreparer(
            executable=activation_preparer,
            capability_root=WORKSPACE,
            activation_root=package_root,
            runtime_cell_id=WORKBENCH_CELL_ID,
            timeout_seconds=180,
            publish_canonical_empty=True,
        )
        preparer.prepare(captured_input_digest=captured_digest, **captured)
    except HeadlessApiError as error:
        raise WorkbenchError(str(error)) from error
    _regular_file(package_root / BOOTSTRAP_ADMISSION, "ROS canonical-empty admission")
    return package_root


def _regular_file(path: Path, label: str, *, executable: bool = False) -> None:
    if path.is_symlink() or not path.is_file():
        raise WorkbenchError(f"{label} is not a regular file: {path}")
    if executable and not os.access(path, os.X_OK):
        raise WorkbenchError(f"{label} is not executable: {path}")


def _remove_stale_socket(path: Path, label: str) -> None:
    """Remove one named ephemeral socket without touching persistent state."""

    try:
        mode = path.lstat().st_mode
    except FileNotFoundError:
        return
    if not stat.S_ISSOCK(mode):
        raise WorkbenchError(f"{label} is not a socket: {path}")
    path.unlink()


def _ensure_frontend_dependencies() -> None:
    lock_path = FRONTEND_ROOT / "package-lock.json"
    marker = FRONTEND_ROOT / "node_modules/.manyforge-lock.sha256"
    digest = hashlib.sha256(lock_path.read_bytes()).hexdigest()
    if marker.is_file() and marker.read_text(encoding="utf-8").strip() == digest:
        return
    _run(("npm", "ci"), cwd=FRONTEND_ROOT)
    marker.write_text(digest + "\n", encoding="utf-8")


def _port_available(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            listener.bind(("127.0.0.1", port))
        except OSError:
            return False
    return True


def _wait_until(
    description: str,
    predicate: Callable[[], Any],
    processes: Sequence[tuple[str, subprocess.Popen[bytes]]],
    *,
    timeout_seconds: float | None = 20.0,
) -> Any:
    deadline = (
        None if timeout_seconds is None else time.monotonic() + timeout_seconds
    )
    last_error: Exception | None = None
    while deadline is None or time.monotonic() < deadline:
        for name, process in processes:
            if process.poll() is not None:
                raise WorkbenchError(
                    f"{name} exited with status {process.returncode} while waiting "
                    f"for {description}"
                )
        try:
            result = predicate()
            if result:
                return result
        except Exception as error:
            last_error = error
        time.sleep(0.05)
    suffix = f": {last_error}" if last_error is not None else ""
    raise WorkbenchError(f"timed out waiting for {description}{suffix}")


def _manager_projection_ready(status: dict[str, Any], diff: dict[str, Any]) -> bool:
    """Accept only the existing fully converged Manager baseline."""

    source = status.get("source")
    payload = status.get("status")
    runtime = payload.get("runtime") if isinstance(payload, dict) else None
    diff_source = diff.get("source")
    return bool(
        isinstance(source, dict)
        and source.get("available") is True
        and source.get("freshness") == "fresh"
        and source.get("known_gaps") == []
        and isinstance(diff_source, dict)
        and diff_source.get("available") is True
        and diff_source.get("freshness") == "fresh"
        and diff_source.get("known_gaps") == []
        and isinstance(payload, dict)
        and payload.get("manager_ready") is True
        and isinstance(runtime, dict)
        and runtime.get("converged") is True
        and diff.get("converged") is True
    )


def query_status(*, scenario_id: str | None, composer_port: int) -> dict[str, Any]:
    """Read Composer readiness and, when selected, the exact active project."""

    with HeadlessHttpClient(
        f"http://127.0.0.1:{composer_port}", timeout_seconds=1.0
    ) as client:
        health = client.health()
        result = client.request("GET", "/api/scenarios/current")
    if scenario_id is None:
        result = {
            **result,
            "ready": bool(health),
            "unavailable_reasons": (
                [] if health else ["composer_unavailable"]
            ),
        }
    current = result.get("current_scenario")
    if (
        scenario_id is not None
        and isinstance(current, dict)
        and current.get("scenario_id") != scenario_id
    ):
        result = {
            **result,
            "ready": False,
            "current_scenario": None,
            "unavailable_reasons": [
                *result.get("unavailable_reasons", []),
                "requested_scenario_mismatch",
            ],
        }
    return {
        **result,
        "classification": {
            "kind": "development_workbench",
            "package_kind": "repository_source",
            "physical_execution_authorized": False,
        },
    }


def _operator_manager_ready(manager: dict[str, Any]) -> bool:
    source = manager.get("source")
    payload = manager.get("status")
    return (isinstance(source, dict) and isinstance(payload, dict)
            and source.get("available") is True and source.get("freshness") == "fresh"
            and source.get("known_gaps") == [] and payload.get("manager_ready") is True)


def query_operator_status(*, headless_port: int, runner_port: int,
                          composer_port: int) -> dict[str, Any]:
    """Read real owner payloads; timing is never a readiness predicate."""
    with HeadlessHttpClient(f"http://127.0.0.1:{headless_port}", timeout_seconds=1.0) as client:
        headless = client.health()
        manager = client.manager_status()
    with HeadlessHttpClient(f"http://127.0.0.1:{runner_port}", timeout_seconds=1.0) as client:
        runner = client.request("GET", "/api/program/health")
    with HeadlessHttpClient(f"http://127.0.0.1:{composer_port}", timeout_seconds=1.0) as client:
        composer = client.health()
    if not all(isinstance(row, dict) for row in (manager, headless, runner, composer)):
        raise WorkbenchError("owner status must be an object")
    ready = (_operator_manager_ready(manager)
             and headless.get("status") == "ready"
             and headless.get("headless_projection_ready") is True
             and runner.get("started") is True and runner.get("closed") is False
             and composer.get("status") == "ready")
    try:
        timing = json.loads((STATE_ROOT / "current/workbench-startup-timing.json").read_text())
    except (OSError, ValueError):
        timing = None
    return {"ready": ready, "manager": manager, "headless": headless,
            "program_runner": runner, "composer": composer, "timing": timing}


def _startup_cycle_state(client: HeadlessHttpClient) -> str:
    """Accept only contained startup states; terminal state remains resettable."""

    cycle = client.program_cycle_status()
    state = cycle.get("state")
    if state not in {"idle_after_reset", "terminal_awaiting_reset"}:
        raise WorkbenchError(f"Program cycle is not contained at startup: {state!r}")
    return state


class Workbench:
    def __init__(
        self,
        *,
        project_path: str | None,
        headless_port: int,
        runner_port: int,
        composer_port: int,
        frontend_port: int,
        artifact_root: Path | None = None,
        ros2_control: bool = True,
        bootstrap_project_path: str | None = None,
        operator_install_root: Path | None = None,
        operator_cell_id: str | None = None,
        canonical_empty_admission_filename: str | None = None,
        composer_static_dir: Path | None = None,
        composer_bind_host: str = "127.0.0.1",
        simulation_control_config: Path | None = None,
        site_settings: Path | None = None,
        package_aggregate_digest: str | None = None,
        operator_instance_id: str | None = None,
        node_catalog_path: Path | None = None,
        alarm_catalog_path: Path | None = None,
        operator_readiness_timeout: float = 60.0,
    ) -> None:
        self.project_path = project_path
        self.headless_port = headless_port
        self.runner_port = runner_port
        self.composer_port = composer_port
        self.frontend_port = frontend_port
        self.artifact_root = artifact_root or WORKSPACE / "examples"
        self.ros2_control = ros2_control
        self.bootstrap_project_path = bootstrap_project_path
        self.operator_install_root = operator_install_root
        self.operator_cell_id = operator_cell_id
        self.operator_admission = canonical_empty_admission_filename
        self.composer_static_dir = composer_static_dir
        self.composer_bind_host = composer_bind_host
        self.simulation_control_config = simulation_control_config
        self.site_settings = site_settings
        self.package_aggregate_digest = package_aggregate_digest
        self.operator_instance_id = operator_instance_id
        self.node_catalog_path = node_catalog_path or NODE_CATALOG
        self.alarm_catalog_path = alarm_catalog_path or ALARM_CATALOG
        if not 0 < operator_readiness_timeout <= 600:
            raise WorkbenchError("operator readiness timeout must be in (0, 600]")
        self.operator_readiness_timeout = operator_readiness_timeout
        self._readiness_deadline: float | None = None
        self.scenario_id: str | None = None
        self.processes: list[tuple[str, subprocess.Popen[bytes]]] = []
        self.stop_requested = False
        self.session: Session | None = None
        self.runtime_root = STATE_ROOT / "current"

    def _spawn(
        self,
        name: str,
        command: Sequence[str],
        *,
        cwd: Path | None = None,
    ) -> None:
        print(f"starting {name}: {' '.join(command)}", flush=True)
        process = subprocess.Popen(command, cwd=cwd, env=os.environ.copy())
        self.processes.append((name, process))

    def _paths(self) -> dict[str, Path]:
        paths = {
            "manager_state": self.runtime_root / "manager",
            "manager_run": self.runtime_root / "manager-run",
            "execution_state": self.runtime_root / "execution-state",
            "execution_run": self.runtime_root / "execution-run",
            "publications": self.runtime_root / "publications",
            "artifacts": self.runtime_root / "artifacts",
            "composer_authoring": self.runtime_root / "composer-authoring",
        }
        for path in paths.values():
            path.mkdir(parents=True, exist_ok=True)
        if self.ros2_control:
            # One runtime-wide ROS home preserves controller-manager spawner
            # lock coordination across controllers, replacements and epochs.
            # Controller child logs still use their existing artifact root.
            ros_home = paths["execution_run"] / "ros-home"
            ros_home.mkdir(parents=True, exist_ok=True)
            os.environ["ROS_HOME"] = str(ros_home)
        return paths

    def _composer_command(self, base_url: str) -> list[str]:
        command = [
            sys.executable,
            "-m",
            "manyforge_composer",
            "--host",
            self.composer_bind_host,
            "--port",
            str(self.composer_port),
            "--headless-api-url",
            base_url,
            "--headless-api-timeout-s",
            "20",
            "--assistant-bridge-url",
            f"http://127.0.0.1:{ASSISTANT_BRIDGE_PORT}",
        ]
        if self.composer_static_dir is not None:
            command.extend(("--static-dir", str(self.composer_static_dir)))
        if self.project_path is not None:
            command.extend(("--startup-scenario-document", self.project_path))
        return command

    def _assistant_bridge_environment(self, base_url: str) -> dict[str, str]:
        # The bridge always runs; it serves only when the active Deployment has
        # an AI model component, whose endpoint it reads per request.
        environment = os.environ.copy()
        environment.update(
            {
                "MANYFORGE_HEADLESS_API_URL": base_url,
                "MANYFORGE_COMPOSER_URL": f"http://127.0.0.1:{self.composer_port}",
                "MANYFORGE_ASSISTANT_BRIDGE_HOST": "127.0.0.1",
                "MANYFORGE_ASSISTANT_BRIDGE_PORT": str(ASSISTANT_BRIDGE_PORT),
                # Rotated by the bridge at 10 MB x 3 files.
                "MANYFORGE_ASSISTANT_AUDIT_LOG": str(
                    self.runtime_root / "assistant-bridge" / "audit.jsonl"
                ),
            }
        )
        return environment

    def _runtime_targets(self) -> tuple[Path, ...]:
        return _ensure_current_ros_backend()

    def _simulation_control_arguments(self) -> tuple[str, ...]:
        if not self.ros2_control:
            return ()
        config = self.simulation_control_config or WORKSPACE / "manyforge_ros_bringup/config/isaac_program_simulation_site.json"
        _regular_file(config, "Isaac Program simulation site configuration")
        return ("--simulation-control-config", str(config))

    def _wait_until(self, description: str, predicate: Callable[[], Any],
                    processes: Sequence[tuple[str, subprocess.Popen[bytes]]],
                    *, timeout_seconds: float | None = 20.0) -> Any:
        if self._readiness_deadline is not None:
            timeout_seconds = max(0.0, self._readiness_deadline - time.monotonic())
        return _wait_until(description, predicate, processes, timeout_seconds=timeout_seconds)

    def _site_manager_arguments(self) -> tuple[str, ...]:
        # Machine facts only (manyforge.site.v1); the Manager validates the file.
        if self.site_settings is None:
            return ()
        _regular_file(self.site_settings, "site settings")
        return ("--site-settings", str(self.site_settings))
    def start(self) -> None:
        if not self.ros2_control:
            raise WorkbenchError("workbench requires ROS control; native execution is unavailable")
        if self.bootstrap_project_path is not None and (
            not self.ros2_control or self.project_path is not None
        ):
            raise WorkbenchError("--bootstrap-project requires ROS control and excludes --project")
        operator_mode = self.operator_install_root is not None
        if operator_mode:
            if not self.ros2_control or self.project_path or self.bootstrap_project_path:
                raise WorkbenchError("operator install requires ROS and excludes project auto-Load")
            if not self.operator_cell_id or not self.operator_admission:
                raise WorkbenchError("operator install requires its original cell/admission identity")
            if Path(self.operator_admission).name != self.operator_admission:
                raise WorkbenchError("operator admission must be package-relative")
            from manyforge_operator.package_aggregate import package_aggregate_sha256
            if package_aggregate_sha256(self.operator_install_root) != self.package_aggregate_digest:
                raise WorkbenchError("qualified operator package aggregate mismatch")
            _regular_file(self.operator_install_root / self.operator_admission, "qualified operator admission")
            if self.composer_static_dir is not None:
                _regular_file(self.composer_static_dir / "index.html", "built Composer frontend")
        ros_capture: dict[str, Any] | None = None
        if self.ros2_control and not operator_mode:
            intended_project = self.bootstrap_project_path or self.project_path
            if intended_project is not None:
                ros_capture, source_path = _capture_ros_bootstrap_project(
                    intended_project, allowed_root=self.artifact_root
                )
                if self.project_path is not None:
                    self.project_path = source_path
                    self.scenario_id = ros_capture["scenario_id"]
        for port in (
            self.headless_port,
            self.runner_port,
            self.composer_port,
            ASSISTANT_BRIDGE_PORT,
            *((self.frontend_port,) if not operator_mode else ()),
        ):
            if not _port_available(port):
                raise WorkbenchError(
                    f"required development port {port} is already in use"
                )
        self.runtime_root.mkdir(parents=True, exist_ok=True)
        timing_path = self.runtime_root / "workbench-startup-timing.json"
        timing_path.write_text(_json({"build_elapsed_seconds": None,
                                      "readiness_elapsed_seconds": None}) + "\n", encoding="utf-8")
        build_started = time.monotonic()
        current_headless_extension = _ensure_current_backend()
        current_runtime_targets = self._runtime_targets()
        activation_preparer = _ensure_activation_preparer()
        bootstrap_root: Path
        bootstrap_admission = self.operator_admission or BOOTSTRAP_ADMISSION
        cell_id = self.operator_cell_id or WORKBENCH_CELL_ID
        bootstrap_root = self.operator_install_root or (
            _ensure_ros_bootstrap_package(ros_capture, activation_preparer=activation_preparer)
            if ros_capture is not None else _ensure_repository_bootstrap_package()
        )
        _regular_file(
            MANAGER_BINARY,
            "current Manager executable",
            executable=True,
        )
        _regular_file(
            current_headless_extension,
            "current Headless Python extension",
        )
        os.environ["PYTHONPATH"] = (
            f"{HEADLESS_BUILD}:{os.environ.get('PYTHONPATH', '')}"
        )
        if not operator_mode:
            _ensure_frontend_dependencies()
        paths = self._paths()
        # Timing is diagnostic only; status always reads the actual owners.
        timing = {"build_elapsed_seconds": time.monotonic() - build_started,
                  "readiness_elapsed_seconds": None}
        timing_path = self.runtime_root / "workbench-startup-timing.json"
        timing_path.write_text(_json(timing) + "\n", encoding="utf-8")
        readiness_started = time.monotonic()
        if operator_mode:
            self._readiness_deadline = readiness_started + self.operator_readiness_timeout
        installed = self.runtime_root / "installed-targets"
        installed.mkdir(parents=True, exist_ok=True)
        manager = installed / "manyforge_manager_daemon"
        shutil.copy2(MANAGER_BINARY, manager)
        _regular_file(manager, "installed Manager", executable=True)
        _stage_runtime_targets(current_runtime_targets, installed)
        manager_socket = paths["manager_run"] / "manager-command.sock"
        runner_socket = paths["manager_run"] / "program-runner-liveness.sock"
        _remove_stale_socket(manager_socket, "Manager command socket")
        _remove_stale_socket(runner_socket, "Program runner liveness socket")
        if ros_capture is not None:
            current_capture, _ = _capture_ros_bootstrap_project(
                source_path, allowed_root=self.artifact_root
            )
            if current_capture != ros_capture:
                raise WorkbenchError(
                    "intended first ROS Scenario changed during startup preparation; "
                    "no runtime was started"
                )
        if self.ros2_control:
            # One actual workbench session owns host logs. Controller children
            # retain their per-artifact override; ROS_HOME remains runtime-wide.
            # Session-local lock, distinct from the installation root flock
            # held by this launcher for its entire owner lifetime.
            ros_logs = create_host_ros_log_session(self.runtime_root)
            os.environ["ROS_LOG_DIR"] = str(ros_logs)
        # Bring up the two reconciliation services before Manager.  A new
        # Manager must be able to reconcile the retained Program Runner owner
        # on its first heartbeat; starting Manager first creates a needless
        # dependency cycle in which its native child can expire while the
        # Runner is still waiting for Headless cell-admission recovery.
        self._spawn(
            "Program runner",
            (
                sys.executable,
                "-m",
                "manyforge_program_runner",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.runner_port),
                "--headless-url",
                f"http://127.0.0.1:{self.headless_port}",
                "--execution-run-root",
                str(paths["execution_run"]),
                "--cycle-journal-path",
                str(self.runtime_root / "program-cycle.sqlite3"),
                "--manager-liveness-socket",
                str(runner_socket),
                "--cell-id",
                cell_id,
                "--owner-id",
                f"program-runner.{self.operator_instance_id or 'development-workbench'}",
                "--persistent-key-register-path",
                str(_persistent_key_register_path()),
                *self._simulation_control_arguments(),
            ),
        )
        self._spawn(
            "Headless API",
            (
                sys.executable,
                "-m",
                "manyforge_headless_api",
                "serve",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.headless_port),
                "--projection-root",
                str(paths["publications"]),
                "--install-root",
                str(bootstrap_root),
                "--canonical-empty-admission-filename",
                str(bootstrap_admission),
                "--artifact-root",
                str(paths["artifacts"]),
                "--node-catalog-path",
                str(self.node_catalog_path),
                "--node-environment-manifest-path",
                str(NODE_ENVIRONMENT_MANIFEST),
                "--alarm-catalog-path",
                str(self.alarm_catalog_path),
                "--historian-database-path",
                str(STATE_ROOT / "history.sqlite3"),
                *self._previous_session_arguments(),
                "--manager-run-root",
                str(paths["manager_run"]),
                "--execution-run-root",
                str(paths["execution_run"]),
                "--cell-id",
                cell_id,
                "--owner-id",
                f"headless-api.{self.operator_instance_id or 'development-workbench'}",
                "--freshness-ms",
                "2000",
                "--manager-timeout-s",
                "15",
                "--program-runner-url",
                f"http://127.0.0.1:{self.runner_port}",
                "--activation-preparer",
                str(activation_preparer),
                "--activation-preparer-capability-root",
                str(WORKSPACE),
                "--activation-preparer-activation-root",
                str(paths["artifacts"]),
            ),
        )
        base_url = f"http://127.0.0.1:{self.headless_port}"

        def reconciliation_services_ready() -> bool:
            with HeadlessHttpClient(
                f"http://127.0.0.1:{self.runner_port}", timeout_seconds=0.5
            ) as client:
                runner = client.request("GET", "/api/program/health")
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                api = client.runtime()
            return bool(runner.get("started") is True and api.get("schema_version"))

        self._wait_until(
            "Program Runner and Headless reconciliation services",
            reconciliation_services_ready,
            self.processes,
        )
        self._spawn(
            "Manager",
            (
                str(manager),
                "--install-root",
                str(bootstrap_root),
                "--activation-root",
                str(paths["artifacts"]),
                "--manager-state-root",
                str(paths["manager_state"]),
                "--manager-run-root",
                str(paths["manager_run"]),
                "--execution-state-root",
                str(paths["execution_state"]),
                "--execution-run-root",
                str(paths["execution_run"]),
                "--logical-scene-database",
                str(self.runtime_root / "logical-scene.sqlite3"),
                "--program-cycle-journal",
                str(self.runtime_root / "program-cycle.sqlite3"),
                "--projection-root",
                str(paths["publications"]),
                "--canonical-empty-admission-filename",
                bootstrap_admission,
                "--program-runner-liveness-socket",
                str(runner_socket),
                # Development value only: relative Program scene meshes such
                # as examples/<name>/meshes/*.obj resolve below the checkout.
                "--scene-asset-root",
                str(WORKSPACE),
            ) + self._site_manager_arguments(),
        )
        self._wait_until(
            "Manager command socket",
            manager_socket.is_socket,
            self.processes,
        )

        # Projection rebuild and composition materialization are asynchronous.
        # Their total duration depends on the retained model and host load, so
        # keep waiting while the managed processes remain alive; do not turn a
        # slow but healthy operation into a startup failure.
        while True:
            if self._readiness_deadline is not None and time.monotonic() >= self._readiness_deadline:
                raise WorkbenchError("timed out waiting for fresh Manager readiness")
            for name, process in self.processes:
                if process.poll() is not None:
                    raise WorkbenchError(
                        f"{name} exited with status {process.returncode} while "
                        "waiting for fresh converged Manager baseline"
                    )
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                status = client.manager_status()
                diff = {} if operator_mode else client.manager_diff()
            if (_operator_manager_ready(status) if operator_mode else _manager_projection_ready(status, diff)):
                break
            # Manager reconciliation itself runs at 500 ms. Polling faster
            # only reserializes two large status documents and competes with
            # the projection work without discovering completion sooner.
            time.sleep(0.5)

        def contained_startup_cycle() -> str | None:
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                try:
                    return _startup_cycle_state(client)
                except WorkbenchError:
                    # Manager rebuilds the exact retained native product on its
                    # existing asynchronous builder lane.  A replacement Runner
                    # stays fenced until that product can certify containment;
                    # treat the intervening active state as startup progress,
                    # never as permission to continue the old run.
                    return None

        if not operator_mode:
            self._wait_until(
                "contained Program-cycle startup reconciliation",
                contained_startup_cycle,
                self.processes,
                timeout_seconds=None,
            )

        print("starting Assistant bridge", flush=True)
        bridge = subprocess.Popen(
            [sys.executable, "-m", "manyforge_assistant_bridge.bridge"],
            env=self._assistant_bridge_environment(base_url),
        )
        self.processes.append(("Assistant bridge", bridge))

        composer_environment = os.environ.copy()
        previous_report = self._previous_session_report()
        if previous_report is not None:
            composer_environment["MANYFORGE_WORKBENCH_PREVIOUS_SESSION_REPORT"] = str(previous_report)
        else:
            composer_environment.pop("MANYFORGE_WORKBENCH_PREVIOUS_SESSION_REPORT", None)
        composer_environment["MANYFORGE_CORS_ORIGINS"] = (
            f"http://localhost:{self.frontend_port},"
            f"http://127.0.0.1:{self.frontend_port}"
        )
        composer_roots = _composer_filesystem_roots(self.artifact_root)
        composer_environment["MANYFORGE_FS_ROOTS"] = composer_roots
        composer_environment["MANYFORGE_PROGRAM_WRITE_ROOTS"] = composer_roots
        composer_environment["MANYFORGE_DEPLOYMENT_WRITE_ROOTS"] = composer_roots
        print("starting Composer backend", flush=True)
        # Composer owns only process-lifetime Draft state. Runtime transitions
        # are durable Manager operations and need no Composer state root.
        # Installation presentation settings are the one exception: they
        # persist under the inherited MANYFORGE_STATE_DIR (composer/).
        composer_command = self._composer_command(base_url)
        composer = subprocess.Popen(
            composer_command,
            env=composer_environment,
        )
        self.processes.append(("Composer backend", composer))

        def composer_ready() -> bool:
            with HeadlessHttpClient(
                f"http://127.0.0.1:{self.composer_port}", timeout_seconds=0.5
            ) as client:
                return bool(client.health())

        self._wait_until(
            "Composer backend",
            composer_ready,
            self.processes,
            timeout_seconds=None,
        )

        if self.scenario_id is not None:
            def exact_status() -> dict[str, Any] | None:
                result = query_status(
                    scenario_id=self.scenario_id,
                    composer_port=self.composer_port,
                )
                return result if result["ready"] else None

            self._wait_until(
                "exact current Scenario project",
                exact_status,
                self.processes,
                timeout_seconds=None,
            )
        if operator_mode:
            timing["readiness_elapsed_seconds"] = time.monotonic() - readiness_started
            timing_path.write_text(_json(timing) + "\n", encoding="utf-8")
            print(_json({"ready": True, "url": f"http://localhost:{self.composer_port}",
                         "timing": timing}), flush=True)
            return
        vite_environment = os.environ.copy()
        vite_environment["MANYFORGE_COMPOSER_PROXY_TARGET"] = (
            f"http://127.0.0.1:{self.composer_port}"
        )
        print("starting Vite HMR", flush=True)
        vite = subprocess.Popen(
            (
                "node",
                str(FRONTEND_ROOT / "node_modules/vite/bin/vite.js"),
                "--host",
                "0.0.0.0",
                "--port",
                str(self.frontend_port),
            ),
            cwd=FRONTEND_ROOT,
            env=vite_environment,
        )
        self.processes.append(("Vite HMR", vite))

        def frontend_ready() -> bool:
            with socket.create_connection(
                ("127.0.0.1", self.frontend_port), timeout=0.5
            ):
                return True

        self._wait_until("Vite HMR", frontend_ready, self.processes)
        print(
            _json(
                {
                    "ready": True,
                    "url": f"http://localhost:{self.frontend_port}",
                    "scenario_id": self.scenario_id,
                    "project_path": self.project_path,
                    "classification": (
                        "development workbench; repository source; "
                        "simulated execution only"
                    ),
                }
            ),
            flush=True,
        )

    def _previous_session_report(self) -> Path | None:
        previous = self.session.marker.get("previous_session") if self.session is not None else None
        return STATE_ROOT / "session-reports" / (previous["session_id"] + ".json") if previous else None

    def _previous_session_arguments(self) -> tuple[str, ...]:
        if self.session is None:
            return ()
        arguments = ("--workbench-previous-session-reports-root", str(STATE_ROOT / "session-reports"))
        previous = self.session.marker.get("previous_session")
        return arguments + (("--workbench-previous-session-id", previous["session_id"]) if previous else ())

    def _shutdown_manager(self) -> bool:
        manager = next(
            (process for name, process in self.processes if name == "Manager"),
            None,
        )
        if manager is None or manager.poll() is not None:
            return False
        request = (
            b'{"api_version":"manyforge.manager_command.v1",'
            b'"command":"shutdown_runtime"}'
        )
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(1.0)
                client.connect(str(self.runtime_root / "manager-run/manager-command.sock"))
                client.sendall(request)
                response = client.recv(16 * 1024)
                receipt = json.loads(response)
                if not isinstance(receipt, dict) or receipt.get("ok") is not True or receipt != {
                    "api_version": "manyforge.manager_command_response.v1",
                    "manager_source_generation": None,
                    "ok": True,
                    "operation": "shutdown_runtime",
                    "scene_source_generation": None,
                    "scene_runtime_source_generation": None,
                }:
                    raise WorkbenchError("Manager rejected bounded shutdown")
            manager.wait(timeout=5)
            return manager.returncode == 0
        except (OSError, ValueError, subprocess.TimeoutExpired, WorkbenchError):
            return False

    def _stop_manager(self) -> bool:
        """Stop Manager before waiting for the Runner's short lease."""

        contained = self._shutdown_manager()
        manager = next(
            (process for name, process in self.processes if name == "Manager"),
            None,
        )
        if manager is not None:
            # The bounded command is preferred, but a failed command must not
            # leave Manager renewing the lease until the container disappears.
            self._terminate(manager)
        return contained

    def _wait_for_program_runner_manager_expiry(self) -> bool:
        """Let the runner durably observe Manager loss before it exits."""

        runner = next(
            (process for name, process in self.processes if name == "Program runner"),
            None,
        )
        if runner is None or runner.poll() is not None:
            return False

        def manager_expired() -> bool:
            with HeadlessHttpClient(
                f"http://127.0.0.1:{self.runner_port}", timeout_seconds=0.2
            ) as client:
                status = client.request("GET", "/api/program/liveness/status")
                cycle = client.program_cycle_status()
            return bool(
                status.get("state") == "expired"
                and status.get("command_fenced") is True
                and cycle.get("state")
                in {"idle_after_reset", "terminal_awaiting_reset"}
            )

        try:
            _wait_until(
                "Program Runner Manager-loss retention",
                manager_expired,
                (("Program runner", runner),),
                timeout_seconds=2.0,
            )
            return True
        except WorkbenchError:
            # Cleanup remains bounded even if the runner is already unhealthy.
            # Its next start will retain/fence the predecessor through the same
            # watchdog before accepting a replacement Manager.
            return False

    @staticmethod
    def _terminate(process: subprocess.Popen[bytes]) -> bool:
        if process.poll() is not None:
            return True
        process.terminate()
        try:
            process.wait(timeout=5)
            return True
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
            return False

    def close(self) -> None:
        verified = self.stop_requested and all(process.poll() is None for _, process in self.processes)
        for name in ("Vite HMR", "Composer backend"):
            for process_name, process in reversed(self.processes):
                if process_name == name:
                    verified = self._terminate(process) and verified
        # Stop the alarm owner before the providers it evaluates. Headless
        # treats Manager sources going stale after a complete baseline as an
        # unexplained provider loss; stopping Manager first raised one
        # provider_unavailable alarm on every workbench stop.
        for process_name, process in reversed(self.processes):
            if process_name == "Headless API":
                verified = self._terminate(process) and verified
        verified = self._stop_manager() and verified
        verified = self._wait_for_program_runner_manager_expiry() and verified
        for process_name, process in reversed(self.processes):
            if process_name == "Program runner":
                verified = self._terminate(process) and verified
        for name, process in reversed(self.processes):
            if name not in {
                "Vite HMR",
                "Composer backend",
                "Program runner",
                "Manager",
                "Headless API",
            }:
                verified = self._terminate(process) and verified
        if self.session is not None:
            try:
                self.session.stopped(verified=verified)
            finally:
                self.session.close()
                self.session = None

    def serve(self) -> None:
        def request_stop(_signum: int, _frame: Any) -> None:
            self.stop_requested = True

        signal.signal(signal.SIGTERM, request_stop)
        signal.signal(signal.SIGINT, request_stop)
        try:
            STATE_ROOT.mkdir(parents=True, exist_ok=True)
            selected = os.environ.get("MANYFORGE_DEV_COLD_SESSION_ID")
            self.session = Session(STATE_ROOT, selected) if selected else Session.direct(STATE_ROOT)
            self.start()
            while not self.stop_requested:
                for name, process in self.processes:
                    if process.poll() is not None:
                        raise WorkbenchError(
                            f"{name} exited unexpectedly with status "
                            f"{process.returncode}"
                        )
                time.sleep(0.2)
        finally:
            self.close()


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="dev-workbench.py")
    subcommands = parser.add_subparsers(dest="command", required=True)
    for name in ("serve", "status"):
        command = subcommands.add_parser(name)
        projects = command.add_mutually_exclusive_group()
        projects.add_argument("--project")
        command.add_argument("--headless-port", type=int, default=9010)
        command.add_argument("--runner-port", type=int, default=9011)
        command.add_argument("--composer-port", type=int, default=9020)
        command.add_argument("--frontend-port", type=int, default=3000)
        command.add_argument("--operator-runtime", action="store_true")
        if name == "serve":
            projects.add_argument(
                "--bootstrap-project",
                help="select the first ROS Scenario for empty startup, without loading it",
            )
            command.add_argument("--operator-install-root", type=Path)
            command.add_argument("--operator-cell-id")
            command.add_argument("--operator-instance-id")
            command.add_argument("--operator-readiness-timeout", type=float, default=60.0)
            command.add_argument("--node-catalog-path", type=Path)
            command.add_argument("--alarm-catalog-path", type=Path)
            command.add_argument("--canonical-empty-admission-filename")
            command.add_argument("--composer-static-dir", type=Path)
            command.add_argument("--composer-bind-host", default="127.0.0.1")
            command.add_argument("--simulation-control-config", type=Path)
            command.add_argument("--site-settings", type=Path)
            command.add_argument("--package-aggregate-sha256")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "status" and args.operator_runtime:
            result = query_operator_status(headless_port=args.headless_port,
                                           runner_port=args.runner_port,
                                           composer_port=args.composer_port)
            print(_json(result))
            return 0 if result["ready"] else 69
        if args.command == "status":
            scenario_id = None
            if args.project is not None:
                try:
                    scenario_id = _project_scenario_id(
                        args.project,
                        allowed_root=WORKSPACE / "examples",
                    )
                except (OSError, ValueError) as error:
                    raise WorkbenchError(str(error)) from error
            result = query_status(
                scenario_id=scenario_id,
                composer_port=args.composer_port,
            )
            print(_json(result))
            return 0 if result["ready"] else 69
        if args.operator_runtime != (args.operator_install_root is not None):
            raise WorkbenchError("operator runtime requires its qualified install root and explicit startup mode")
        Workbench(
            project_path=args.project,
            headless_port=args.headless_port,
            runner_port=args.runner_port,
            composer_port=args.composer_port,
            frontend_port=args.frontend_port,
            bootstrap_project_path=args.bootstrap_project,
            operator_install_root=args.operator_install_root,
            operator_cell_id=args.operator_cell_id,
            canonical_empty_admission_filename=args.canonical_empty_admission_filename,
            composer_static_dir=args.composer_static_dir,
            composer_bind_host=args.composer_bind_host,
            simulation_control_config=args.simulation_control_config,
            site_settings=args.site_settings,
            package_aggregate_digest=args.package_aggregate_sha256,
            operator_instance_id=args.operator_instance_id,
            node_catalog_path=args.node_catalog_path,
            alarm_catalog_path=args.alarm_catalog_path,
            operator_readiness_timeout=args.operator_readiness_timeout,
        ).serve()
        return 0
    except (HeadlessClientError, OSError, ValueError, WorkbenchError) as error:
        print(f"development_workbench_unavailable: {error}", file=sys.stderr)
        return 69


if __name__ == "__main__":
    raise SystemExit(main())
