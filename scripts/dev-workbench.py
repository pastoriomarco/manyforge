#!/usr/bin/env python3
"""D0 development workbench orchestration inside one dev container."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
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
# entrypoint. Keep this start/status tool self-contained in a source mount.
for _source_root in (
    "/workspace",
    "/workspace/manyforge_behavior",
    "/workspace/manyforge_ros",
):
    if _source_root not in sys.path:
        sys.path.insert(0, _source_root)

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


WORKSPACE = Path("/workspace")
CACHE_ROOT = Path("/runner")
BUILD_ROOT = CACHE_ROOT / "build"
STATE_ROOT = Path("/tmp/manyforge-state")
HEADLESS_BUILD = BUILD_ROOT / "headless"
SCENARIO_ROOT = CACHE_ROOT / "maintained-scenario-package"
PACKAGE_ROOT = SCENARIO_ROOT / "retained-package"
NATIVE_EXECUTION_HOST = SCENARIO_ROOT / "manyforge_native_execution_host"
NATIVE_CANDIDATE_WORKER = SCENARIO_ROOT / "manyforge_roboplan_candidate_worker"
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


def _json(value: Any) -> str:
    return json.dumps(value, allow_nan=False, indent=2, sort_keys=True)


def _run(command: Sequence[str], *, cwd: Path | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode != 0:
        raise WorkbenchError(
            f"command failed with exit {completed.returncode}: {' '.join(command)}"
        )


def _qualified_root() -> Path:
    return Path(
        os.environ.get("MANYFORGE_DEV_QUALIFIED_ROOT", "/manyforge-qualified")
    )


def _import_package() -> None:
    """Import one qualified package and its exact package-owned executables."""

    if (
        PACKAGE_ROOT.is_dir()
        and NATIVE_EXECUTION_HOST.is_file()
        and NATIVE_CANDIDATE_WORKER.is_file()
    ):
        return
    qualified = _qualified_root()
    source_package = qualified / "maintained-package"
    source_native = qualified / "native-install/manyforge_native_execution_host"
    source_worker = qualified / "native-install/manyforge_roboplan_candidate_worker"
    for path, label in (
        (source_native, "qualified native execution host"),
        (source_worker, "qualified candidate worker"),
    ):
        _regular_file(path, label, executable=True)
    if not source_package.is_dir():
        raise WorkbenchError(
            "the maintained package is unavailable; set "
            "MANYFORGE_DEV_QUALIFIED_BUILD_ROOT to an existing qualified build root"
        )
    if SCENARIO_ROOT.exists():
        raise WorkbenchError(
            "maintained package cache is incomplete; inspect or remove "
            f"{SCENARIO_ROOT}"
        )
    temporary = CACHE_ROOT / f"maintained-scenario.import-{os.getpid()}"
    temporary.mkdir(parents=True)
    try:
        shutil.copytree(source_package, temporary / "retained-package")
        shutil.copy2(source_native, temporary / NATIVE_EXECUTION_HOST.name)
        shutil.copy2(source_worker, temporary / NATIVE_CANDIDATE_WORKER.name)
        temporary.replace(SCENARIO_ROOT)
    except Exception:
        shutil.rmtree(temporary, ignore_errors=True)
        raise


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
            raise WorkbenchError("cannot resolve the development pybind11 CMake package")
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
    jobs = os.environ.get("MANYFORGE_DEV_BUILD_JOBS", "").strip()
    if not jobs:
        jobs = str(max(1, os.cpu_count() or 1))
    if not jobs.isdecimal() or int(jobs) < 1:
        raise WorkbenchError("MANYFORGE_DEV_BUILD_JOBS must be a positive integer")
    _run(
        (
            "cmake",
            "--build",
            str(HEADLESS_BUILD),
            "--parallel",
            jobs,
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
        raise WorkbenchError(
            "current Headless Python extension is absent or ambiguous"
        )
    _regular_file(extensions[0], "current Headless Python extension")
    return extensions[0]


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


def _load_scenario(scenario_id: str) -> dict[str, Any]:
    catalog_path = PACKAGE_ROOT / "maintained.scenario-catalog.yaml"
    _regular_file(catalog_path, "maintained Scenario catalog")
    try:
        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        build_manifests = list(
            (PACKAGE_ROOT / "build-manifests/compiler_build").glob("*.json")
        )
        if len(build_manifests) != 1:
            raise ValueError("package must contain one compiler build manifest")
        build_manifest = json.loads(
            build_manifests[0].read_text(encoding="utf-8")
        )
        build_profile = build_manifest["build_profile"]
        if not isinstance(build_profile, str):
            raise ValueError("compiler build profile must be text")
        rows = [
            row
            for row in catalog["scenarios"]
            if row["scenario_id"] == scenario_id
            or scenario_id in row.get("aliases", [])
        ]
        if len(rows) != 1:
            raise ValueError("Scenario is absent or ambiguous")
        row = rows[0]
        if row["deployment_compatibility"]["lane"] != "manyforge-only":
            raise ValueError("Scenario is not a native ManyForge lane")
        binding_path = (
            PACKAGE_ROOT
            / "program-bindings/sha256"
            / f"{row['program_binding_id'].removeprefix('sha256:')}.json"
        )
        bundle_path = (
            PACKAGE_ROOT
            / "retained-bundles/sha256"
            / f"{row['retained_bundle_id'].removeprefix('sha256:')}.json"
        )
        _regular_file(binding_path, "Scenario Program Binding")
        _regular_file(bundle_path, "Scenario retained bundle")
        binding = json.loads(binding_path.read_text(encoding="utf-8"))
        bundle = json.loads(bundle_path.read_text(encoding="utf-8"))
        program_path = (
            PACKAGE_ROOT
            / "programs/sha256"
            / f"{binding['program_revision'].removeprefix('sha256:')}.json"
        )
        _regular_file(program_path, "Scenario Program")
        program = json.loads(program_path.read_text(encoding="utf-8"))
        graph = bundle["cell_graph"]
        runtime_units = graph["runtime_units"]
    except (KeyError, OSError, UnicodeError, ValueError, json.JSONDecodeError) as error:
        raise WorkbenchError(
            f"maintained Scenario {scenario_id!r} is invalid: {error}"
        ) from error
    if (
        binding.get("program_binding_id") != row["program_binding_id"]
        or binding.get("retained_bundle_id") != row["retained_bundle_id"]
        or binding.get("graph_revision") != graph.get("graph_revision")
        or bundle.get("deployment_artifact_id")
        != row["deployment_artifact_id"]
        or bundle.get("deployment_revision") != row["deployment_revision"]
        or program.get("program_id") != binding.get("program_id")
        or program.get("program_revision") != binding.get("program_revision")
        or not isinstance(runtime_units, list)
        or len(runtime_units) != 1
        or runtime_units[0].get("configuration", {}).get("mode") != "simulated"
    ):
        raise WorkbenchError(
            "maintained Scenario artifacts do not form one exact simulated tuple"
        )
    admission_filename = f"canonical-empty-admission.{graph['cell_id']}.json"
    _regular_file(PACKAGE_ROOT / admission_filename, "canonical-empty admission")
    workspace_program = (
        WORKSPACE / "examples" / f"{program['program_id']}.program.yaml"
    )
    _regular_file(workspace_program, "matching workspace Program")
    # Composer parses this artifact and the backend Load Scenario command
    # compares its semantic Program revision with the retained selection.  Do
    # not add a stricter byte-format identity in the process launcher.
    _regular_file(
        NATIVE_EXECUTION_HOST, "native execution host", executable=True
    )
    _regular_file(
        NATIVE_CANDIDATE_WORKER, "candidate worker", executable=True
    )
    return {
        "admission_filename": admission_filename,
        "build_profile": build_profile,
        "cell_id": graph["cell_id"],
        "deployment_artifact_id": row["deployment_artifact_id"],
        "deployment_revision": row["deployment_revision"],
        "graph_revision": graph["graph_revision"],
        "program_binding_id": row["program_binding_id"],
        "program_id": program["program_id"],
        "program_path": str(workspace_program),
        "program_revision": program["program_revision"],
        "retained_bundle_id": row["retained_bundle_id"],
        "scenario_catalog_revision": catalog["scenario_catalog_revision"],
        "scenario_id": row["scenario_id"],
    }


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
    timeout_seconds: float = 20.0,
) -> Any:
    deadline = time.monotonic() + timeout_seconds
    last_error: Exception | None = None
    while time.monotonic() < deadline:
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


def _manager_projection_ready(
    status: dict[str, Any], diff: dict[str, Any]
) -> bool:
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
    """Read Composer's owner-derived current Scenario; retain no launcher copy."""

    with HeadlessHttpClient(
        f"http://127.0.0.1:{composer_port}", timeout_seconds=1.0
    ) as client:
        result = client.request("GET", "/api/scenarios/current")
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
            "package_kind": "maintained_qualified",
            "physical_execution_authorized": False,
        },
    }


class Workbench:
    def __init__(
        self,
        *,
        scenario_id: str,
        headless_port: int,
        runner_port: int,
        composer_port: int,
        frontend_port: int,
    ) -> None:
        self.scenario_id = scenario_id
        self.headless_port = headless_port
        self.runner_port = runner_port
        self.composer_port = composer_port
        self.frontend_port = frontend_port
        self.metadata: dict[str, Any] = {}
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
            "program_selection": STATE_ROOT / "program-selection-state",
            "composer_authoring": STATE_ROOT / "composer-authoring",
        }
        for path in paths.values():
            path.mkdir(parents=True, exist_ok=True)
        return paths

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
        _import_package()
        current_headless_extension = _ensure_current_backend()
        self.metadata = _load_scenario(self.scenario_id)
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
        for source in (NATIVE_EXECUTION_HOST, NATIVE_CANDIDATE_WORKER):
            target = installed / source.name
            shutil.copy2(source, target)
            _regular_file(target, f"installed {source.name}", executable=True)
        manager_socket = paths["manager_run"] / "manager-command.sock"
        runner_socket = paths["manager_run"] / "program-runner-liveness.sock"
        _remove_stale_socket(manager_socket, "Manager command socket")
        _remove_stale_socket(runner_socket, "Program runner liveness socket")
        authoring_journal = (
            paths["composer_authoring"]
            / "composer-authoring-transactions.sqlite3"
        )
        self._spawn(
            "Manager",
            (
                str(manager),
                "--install-root", str(PACKAGE_ROOT),
                "--manager-state-root", str(paths["manager_state"]),
                "--manager-run-root", str(paths["manager_run"]),
                "--execution-state-root", str(paths["execution_state"]),
                "--execution-run-root", str(paths["execution_run"]),
                "--logical-scene-database", str(STATE_ROOT / "logical-scene.sqlite3"),
                "--program-cycle-journal", str(STATE_ROOT / "program-cycle.sqlite3"),
                "--authoring-transaction-journal", str(authoring_journal),
                "--projection-root", str(paths["publications"]),
                "--canonical-empty-admission-filename",
                self.metadata["admission_filename"],
                "--program-runner-liveness-socket", str(runner_socket),
            ),
        )
        _wait_until(
            "Manager command socket",
            manager_socket.is_socket,
            self.processes,
        )
        self._spawn(
            "Program runner",
            (
                sys.executable, "-m", "manyforge_program_runner",
                "--host", "127.0.0.1", "--port", str(self.runner_port),
                "--headless-url", f"http://127.0.0.1:{self.headless_port}",
                "--execution-run-root", str(paths["execution_run"]),
                "--cycle-journal-path", str(STATE_ROOT / "program-cycle.sqlite3"),
                "--manager-liveness-socket", str(runner_socket),
                "--cell-id", self.metadata["cell_id"],
                "--owner-id", "program-runner.development-workbench",
                "--headless-timeout-s", "1",
            ),
        )
        self._spawn(
            "Headless API",
            (
                sys.executable, "-m", "manyforge_headless_api", "serve",
                "--host", "127.0.0.1", "--port", str(self.headless_port),
                "--projection-root", str(paths["publications"]),
                "--install-root", str(PACKAGE_ROOT),
                "--artifact-root", str(paths["artifacts"]),
                "--program-selection-state-root", str(paths["program_selection"]),
                "--node-catalog-path", str(NODE_CATALOG),
                "--node-environment-manifest-path",
                str(NODE_ENVIRONMENT_MANIFEST),
                "--alarm-catalog-path", str(ALARM_CATALOG),
                "--historian-database-path", str(STATE_ROOT / "history.sqlite3"),
                "--manager-run-root", str(paths["manager_run"]),
                "--execution-run-root", str(paths["execution_run"]),
                "--cell-id", self.metadata["cell_id"],
                "--owner-id", "headless-api.development-workbench",
                "--freshness-ms", "2000",
                "--manager-timeout-s", "15",
                "--program-runner-url", f"http://127.0.0.1:{self.runner_port}",
                "--program-runner-timeout-s", "1",
            ),
        )
        base_url = f"http://127.0.0.1:{self.headless_port}"

        def manager_ready() -> dict[str, Any] | None:
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                status = client.manager_status()
                diff = client.manager_diff()
                if _manager_projection_ready(status, diff):
                    return status
            return None

        _wait_until(
            "fresh converged Manager baseline", manager_ready, self.processes
        )

        def authoring_ready() -> dict[str, Any] | None:
            with HeadlessHttpClient(base_url, timeout_seconds=0.5) as client:
                admission = client.authoring_admission()
                return admission if admission.get("authoring_allowed") is True else None

        _wait_until(
            "idle authoring admission", authoring_ready, self.processes
        )
        composer_environment = os.environ.copy()
        composer_environment["MANYFORGE_CORS_ORIGINS"] = (
            f"http://localhost:{self.frontend_port},"
            f"http://127.0.0.1:{self.frontend_port}"
        )
        composer_environment["MANYFORGE_FS_ROOTS"] = str(WORKSPACE / "examples")
        composer_environment["MANYFORGE_PROGRAM_WRITE_ROOTS"] = str(
            WORKSPACE / "examples"
        )
        print("starting Composer backend", flush=True)
        composer = subprocess.Popen(
            (
                sys.executable, "-m", "manyforge_composer",
                "--host", "127.0.0.1", "--port", str(self.composer_port),
                "--program-path", self.metadata["program_path"],
                "--headless-api-url", base_url,
                "--headless-api-timeout-s", "20",
                "--authoring-state-root", str(paths["composer_authoring"]),
                "--startup-scenario", self.scenario_id,
            ),
            env=composer_environment,
        )
        self.processes.append(("Composer backend", composer))

        def composer_ready() -> bool:
            with HeadlessHttpClient(
                f"http://127.0.0.1:{self.composer_port}", timeout_seconds=0.5
            ) as client:
                return bool(client.health())

        _wait_until("Composer backend", composer_ready, self.processes)

        def exact_status() -> dict[str, Any] | None:
            result = query_status(
                scenario_id=self.metadata["scenario_id"],
                composer_port=self.composer_port,
            )
            return result if result["ready"] else None

        _wait_until("exact current Scenario tuple", exact_status, self.processes)
        vite_environment = os.environ.copy()
        vite_environment["MANYFORGE_COMPOSER_PROXY_TARGET"] = (
            f"http://127.0.0.1:{self.composer_port}"
        )
        print("starting Vite HMR", flush=True)
        vite = subprocess.Popen(
            (
                "npm", "run", "dev", "--", "--host", "0.0.0.0",
                "--port", str(self.frontend_port),
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
                    "classification": (
                        "development workbench; maintained qualified package; "
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
        for name in ("Vite HMR", "Composer backend", "Program runner"):
            for process_name, process in reversed(self.processes):
                if process_name == name:
                    self._terminate(process)
        self._shutdown_manager()
        for name, process in reversed(self.processes):
            if name not in {
                "Vite HMR",
                "Composer backend",
                "Program runner",
                "Manager",
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
        command.add_argument("--scenario", required=name == "serve")
        command.add_argument("--headless-port", type=int, default=9010)
        command.add_argument("--runner-port", type=int, default=9011)
        command.add_argument("--composer-port", type=int, default=9020)
        command.add_argument("--frontend-port", type=int, default=3000)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "status":
            result = query_status(
                scenario_id=args.scenario,
                composer_port=args.composer_port,
            )
            print(_json(result))
            return 0 if result["ready"] else 69
        Workbench(
            scenario_id=args.scenario,
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
