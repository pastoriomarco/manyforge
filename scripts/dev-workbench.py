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


WORKSPACE = Path("/workspace")
CACHE_ROOT = Path("/runner")
BUILD_ROOT = CACHE_ROOT / "build"
STATE_ROOT = Path(os.environ.get("MANYFORGE_DEV_STATE_ROOT", "/tmp/manyforge-state"))
HEADLESS_BUILD = BUILD_ROOT / "headless"
ROBOPLAN_BUILD = BUILD_ROOT / "roboplan"
DEPLOYMENT_AUTHORING_BUILD = BUILD_ROOT / "deployment-authoring"
BOOTSTRAP_ROOT = CACHE_ROOT / "repository-bootstrap-package"
# Installation configuration, independent of the optional startup project.
# Preserve the existing workbench identity; never mint one per Load or restart.
WORKBENCH_CELL_ID = "c1_build_authority_cell"
BOOTSTRAP_ADMISSION = f"canonical-empty-admission.{WORKBENCH_CELL_ID}.json"
NATIVE_EXECUTION_HOST = ROBOPLAN_BUILD / "manyforge_native_execution_host"
NATIVE_CANDIDATE_WORKER = ROBOPLAN_BUILD / "manyforge_roboplan_candidate_worker"
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
        value.get("api_version") != "manyforge.scenario.project.v1"
        or not isinstance(scenario_id, str)
        or re.fullmatch(r"[a-z][a-z0-9._-]{0,127}", scenario_id) is None
    ):
        raise WorkbenchError("Scenario project manifest identity is invalid")
    return scenario_id


def _json(value: Any) -> str:
    return json.dumps(value, allow_nan=False, indent=2, sort_keys=True)


def _run(command: Sequence[str], *, cwd: Path | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode != 0:
        raise WorkbenchError(
            f"command failed with exit {completed.returncode}: {' '.join(command)}"
        )


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


def _ensure_current_native_backend() -> tuple[Path, Path]:
    """Incrementally build the current workspace native execution owners."""

    if not (ROBOPLAN_BUILD / "CMakeCache.txt").is_file():
        _run(
            (
                "cmake",
                "-G",
                "Ninja",
                "-S",
                str(WORKSPACE / "manyforge_roboplan"),
                "-B",
                str(ROBOPLAN_BUILD),
                "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                "-DBUILD_TESTING=OFF",
                "-DMANYFORGE_BUILD_ROS2_CONTROL_RUNTIME=OFF",
            )
        )
    _run(
        (
            "cmake",
            "--build",
            str(ROBOPLAN_BUILD),
            "--parallel",
            _build_jobs(),
            "--target",
            "manyforge_native_execution_host",
            "manyforge_roboplan_candidate_worker",
            "--",
            "-k",
            "0",
        )
    )
    _regular_file(
        NATIVE_EXECUTION_HOST, "current native execution host", executable=True
    )
    _regular_file(NATIVE_CANDIDATE_WORKER, "current candidate worker", executable=True)
    return NATIVE_EXECUTION_HOST, NATIVE_CANDIDATE_WORKER


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
    """Materialize native infrastructure without selecting any Scenario."""

    profile = (
        WORKSPACE / "manyforge_deployment/resources/maintained/catalog/"
        "runtime_profile/manyforge.native_execution_runtime.json"
    )
    # Cache placement only, not a semantic artifact identity. Never overwrite
    # an immutable admission created by an older compiler/profile.
    digest = hashlib.sha256(f"native-workbench:{WORKBENCH_CELL_ID}\0".encode())
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
            "--native-runtime-profile",
            str(profile),
        )
    )
    _regular_file(
        package_root / BOOTSTRAP_ADMISSION,
        "repository canonical-empty admission",
    )
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
    ) -> None:
        self.project_path = project_path
        self.headless_port = headless_port
        self.runner_port = runner_port
        self.composer_port = composer_port
        self.frontend_port = frontend_port
        self.artifact_root = artifact_root or WORKSPACE / "examples"
        self.scenario_id: str | None = None
        self.processes: list[tuple[str, subprocess.Popen[bytes]]] = []
        self.stop_requested = False

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
            "manager_state": STATE_ROOT / "manager",
            "manager_run": STATE_ROOT / "manager-run",
            "execution_state": STATE_ROOT / "execution-state",
            "execution_run": STATE_ROOT / "execution-run",
            "publications": STATE_ROOT / "publications",
            "artifacts": STATE_ROOT / "artifacts",
            "composer_authoring": STATE_ROOT / "composer-authoring",
        }
        for path in paths.values():
            path.mkdir(parents=True, exist_ok=True)
        return paths

    def _composer_command(self, base_url: str) -> list[str]:
        command = [
            sys.executable,
            "-m",
            "manyforge_composer",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.composer_port),
            "--headless-api-url",
            base_url,
            "--headless-api-timeout-s",
            "20",
        ]
        if self.project_path is not None:
            command.extend(("--startup-scenario-document", self.project_path))
        return command

    def start(self) -> None:
        for port in (
            self.headless_port,
            self.runner_port,
            self.composer_port,
            self.frontend_port,
        ):
            if not _port_available(port):
                raise WorkbenchError(
                    f"required development port {port} is already in use"
                )
        current_headless_extension = _ensure_current_backend()
        current_native_targets = _ensure_current_native_backend()
        activation_preparer = _ensure_activation_preparer()
        bootstrap_root: Path
        bootstrap_admission = BOOTSTRAP_ADMISSION
        cell_id = WORKBENCH_CELL_ID
        if self.project_path is not None:
            from manyforge_composer.backend.scenario_artifacts import (
                ScenarioArtifactError,
                capture_scenario_artifact,
            )

            try:
                project, project_path, _source_document_digest = capture_scenario_artifact(
                    self.project_path,
                    allowed_roots=[str(self.artifact_root)],
                )
            except ScenarioArtifactError as error:
                raise WorkbenchError(str(error)) from error
            self.project_path = project_path
            self.scenario_id = project["scenario_id"]
        bootstrap_root = _ensure_repository_bootstrap_package()
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
        _ensure_frontend_dependencies()
        paths = self._paths()
        installed = STATE_ROOT / "installed-targets"
        installed.mkdir(parents=True, exist_ok=True)
        manager = installed / "manyforge_manager_daemon"
        shutil.copy2(MANAGER_BINARY, manager)
        _regular_file(manager, "installed Manager", executable=True)
        for source in current_native_targets:
            target = installed / source.name
            shutil.copy2(source, target)
            _regular_file(target, f"installed {source.name}", executable=True)
        manager_socket = paths["manager_run"] / "manager-command.sock"
        runner_socket = paths["manager_run"] / "program-runner-liveness.sock"
        _remove_stale_socket(manager_socket, "Manager command socket")
        _remove_stale_socket(runner_socket, "Program runner liveness socket")
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
                str(STATE_ROOT / "program-cycle.sqlite3"),
                "--manager-liveness-socket",
                str(runner_socket),
                "--cell-id",
                cell_id,
                "--owner-id",
                "program-runner.development-workbench",
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
                "--artifact-root",
                str(paths["artifacts"]),
                "--node-catalog-path",
                str(NODE_CATALOG),
                "--node-environment-manifest-path",
                str(NODE_ENVIRONMENT_MANIFEST),
                "--alarm-catalog-path",
                str(ALARM_CATALOG),
                "--historian-database-path",
                str(STATE_ROOT / "history.sqlite3"),
                "--manager-run-root",
                str(paths["manager_run"]),
                "--execution-run-root",
                str(paths["execution_run"]),
                "--cell-id",
                cell_id,
                "--owner-id",
                "headless-api.development-workbench",
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

        _wait_until(
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
                str(STATE_ROOT / "logical-scene.sqlite3"),
                "--program-cycle-journal",
                str(STATE_ROOT / "program-cycle.sqlite3"),
                "--projection-root",
                str(paths["publications"]),
                "--canonical-empty-admission-filename",
                bootstrap_admission,
                "--program-runner-liveness-socket",
                str(runner_socket),
            ),
        )
        _wait_until(
            "Manager command socket",
            manager_socket.is_socket,
            self.processes,
        )

        # Projection rebuild and composition materialization are asynchronous.
        # Their total duration depends on the retained model and host load, so
        # keep waiting while the managed processes remain alive; do not turn a
        # slow but healthy operation into a startup failure.
        while True:
            for name, process in self.processes:
                if process.poll() is not None:
                    raise WorkbenchError(
                        f"{name} exited with status {process.returncode} while "
                        "waiting for fresh converged Manager baseline"
                    )
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                status = client.manager_status()
                diff = client.manager_diff()
            if _manager_projection_ready(status, diff):
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

        _wait_until(
            "contained Program-cycle startup reconciliation",
            contained_startup_cycle,
            self.processes,
            timeout_seconds=None,
        )

        def authoring_ready() -> dict[str, Any] | None:
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                admission = client.authoring_admission()
                return admission if admission.get("authoring_allowed") is True else None

        composer_environment = os.environ.copy()
        composer_environment["MANYFORGE_CORS_ORIGINS"] = (
            f"http://localhost:{self.frontend_port},"
            f"http://127.0.0.1:{self.frontend_port}"
        )
        composer_environment["MANYFORGE_FS_ROOTS"] = str(self.artifact_root)
        composer_environment["MANYFORGE_PROGRAM_WRITE_ROOTS"] = str(self.artifact_root)
        composer_environment["MANYFORGE_DEPLOYMENT_WRITE_ROOTS"] = str(
            self.artifact_root
        )
        print("starting Composer backend", flush=True)
        # Composer owns only process-lifetime Draft state. Runtime transitions
        # are durable Manager operations and need no Composer state root.
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

        _wait_until(
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

            _wait_until(
                "exact current Scenario project",
                exact_status,
                self.processes,
                timeout_seconds=None,
            )
        _wait_until(
            "idle authoring admission",
            authoring_ready,
            self.processes,
            timeout_seconds=None,
        )
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

        _wait_until("Vite HMR", frontend_ready, self.processes)
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

    def _shutdown_manager(self) -> None:
        manager = next(
            (process for name, process in self.processes if name == "Manager"),
            None,
        )
        if manager is None or manager.poll() is not None:
            return
        request = (
            b'{"api_version":"manyforge.manager_command.v1",'
            b'"command":"shutdown_runtime"}'
        )
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(1.0)
                client.connect(str(STATE_ROOT / "manager-run/manager-command.sock"))
                client.sendall(request)
                response = client.recv(16 * 1024)
                if b'"ok":true' not in response:
                    raise WorkbenchError("Manager rejected bounded shutdown")
            manager.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired, WorkbenchError):
            pass

    def _stop_manager(self) -> None:
        """Stop Manager before waiting for the Runner's short lease."""

        self._shutdown_manager()
        manager = next(
            (process for name, process in self.processes if name == "Manager"),
            None,
        )
        if manager is not None:
            # The bounded command is preferred, but a failed command must not
            # leave Manager renewing the lease until the container disappears.
            self._terminate(manager)

    def _wait_for_program_runner_manager_expiry(self) -> None:
        """Let the runner durably observe Manager loss before it exits."""

        runner = next(
            (process for name, process in self.processes if name == "Program runner"),
            None,
        )
        if runner is None or runner.poll() is not None:
            return

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
        except WorkbenchError:
            # Cleanup remains bounded even if the runner is already unhealthy.
            # Its next start will retain/fence the predecessor through the same
            # watchdog before accepting a replacement Manager.
            pass

    @staticmethod
    def _terminate(process: subprocess.Popen[bytes]) -> None:
        if process.poll() is not None:
            return
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)

    def close(self) -> None:
        for name in ("Vite HMR", "Composer backend"):
            for process_name, process in reversed(self.processes):
                if process_name == name:
                    self._terminate(process)
        self._stop_manager()
        self._wait_for_program_runner_manager_expiry()
        for process_name, process in reversed(self.processes):
            if process_name == "Program runner":
                self._terminate(process)
        for process_name, process in reversed(self.processes):
            if process_name == "Headless API":
                self._terminate(process)
        for name, process in reversed(self.processes):
            if name not in {
                "Vite HMR",
                "Composer backend",
                "Program runner",
                "Manager",
                "Headless API",
            }:
                self._terminate(process)

    def serve(self) -> None:
        def request_stop(_signum: int, _frame: Any) -> None:
            self.stop_requested = True

        signal.signal(signal.SIGTERM, request_stop)
        signal.signal(signal.SIGINT, request_stop)
        try:
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
        command.add_argument("--project")
        command.add_argument("--headless-port", type=int, default=9010)
        command.add_argument("--runner-port", type=int, default=9011)
        command.add_argument("--composer-port", type=int, default=9020)
        command.add_argument("--frontend-port", type=int, default=3000)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
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
        Workbench(
            project_path=args.project,
            headless_port=args.headless_port,
            runner_port=args.runner_port,
            composer_port=args.composer_port,
            frontend_port=args.frontend_port,
        ).serve()
        return 0
    except (HeadlessClientError, OSError, ValueError, WorkbenchError) as error:
        print(f"development_workbench_unavailable: {error}", file=sys.stderr)
        return 69


if __name__ == "__main__":
    raise SystemExit(main())
