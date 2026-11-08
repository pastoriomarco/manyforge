"""Process-boundary qualification for Manager, API, CLI, and Composer."""

from __future__ import annotations

from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
import json
from tempfile import TemporaryDirectory
import hashlib
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time
from typing import Callable, Iterator

import pytest
from manyforge_headless_api.process_tests.control_client_fixture import composer_snapshot_stream

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.manager import ManagerCommandClient
from manyforge_headless_api.models import ManagerDeploymentCandidatePreviewRequest, PreparedScenarioOrigin
from manyforge_headless_api.program_runner_client import ProgramRunnerHttpClient


CATALOG = Path("/workspace/manyforge_headless/resources/operator-alarm-catalog.v1.yaml")
NODE_CATALOG = Path("/workspace/manyforge_behavior/resources/node_catalog.yaml")
NODE_ENVIRONMENT_MANIFEST = Path(
    "/workspace/manyforge_behavior/resources/node_environment_manifest.yaml"
)


def _required_path(name: str) -> Path:
    raw = os.environ.get(name)
    if not raw:
        raise RuntimeError(f"{name} is required for process qualification")
    path = Path(raw)
    if not path.exists():
        raise RuntimeError(f"{name} does not exist: {path}")
    return path


def keep_failure_evidence(root: Path) -> None:
    """Copy a failed stack's logs, controller-launch files and SQLite journals
    out of its temporary root, when the lane names
    MANYFORGE_PROCESS_TEST_EVIDENCE_ROOT."""

    raw = os.environ.get("MANYFORGE_PROCESS_TEST_EVIDENCE_ROOT")
    if not raw:
        return
    test = os.environ.get("PYTEST_CURRENT_TEST", "unknown").split(" ", 1)[0]
    target = Path(raw) / test.replace("/", "_").replace("::", "__")
    copied = 0
    skipped = 0
    readiness_files: list[dict[str, str]] = []
    for path in root.rglob("*"):
        if path.is_file() and not path.is_symlink() and (
            path.suffix in {".log", ".sqlite3", ".sqlite3-wal", ".sqlite3-shm"}
            or path.name.startswith("controller_launch")
            or path.name == "readiness-last-snapshot.json"
        ):
            destination = target / path.relative_to(root)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
            copied += 1
            if path.name == "readiness-last-snapshot.json":
                readiness_files.append({
                    "path": str(path.relative_to(root)),
                    "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
                })
        elif path.is_file() or path.is_symlink():
            skipped += 1
    target.mkdir(parents=True, exist_ok=True)
    (target / "failure-copy-summary.json").write_text(json.dumps({
        "copied_file_count": copied, "skipped_file_count": skipped,
        "readiness_files": readiness_files,
    }, sort_keys=True) + "\n", encoding="utf-8")


def _drain_into(source, destination_fd: int) -> None:
    """Copy one process's piped output into the stack log until EOF."""

    try:
        with source:
            while chunk := source.read1(65536):
                view = memoryview(chunk)
                while view:
                    view = view[os.write(destination_fd, view):]
    except (OSError, ValueError):
        pass
    finally:
        os.close(destination_fd)


def _free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


class ProcessStack:
    def __init__(
        self,
        root: Path,
        *,
        package_root: Path | None = None,
        cell_id: str = "c1_build_authority_cell",
        program_path: Path | None = None,
        composer_fs_roots: tuple[Path, ...] | None = None,
        freshness_milliseconds: int = 200,
        enable_program_runner: bool = False,
    ) -> None:
        self.root = root
        self.manager_root = root / "manager"
        self.manager_run_root = root / "manager-run"
        self.execution_state_root = root / "execution-state"
        self.execution_run_root = root / "execution-run"
        self.publication_root = root / "publications"
        self.artifact_root = root / "artifacts"
        for path in (
            self.manager_root,
            self.manager_run_root,
            self.execution_state_root,
            self.execution_run_root,
            self.publication_root,
            self.artifact_root,
        ):
            path.mkdir()
        self.scene_database = root / "logical-scene.sqlite3"
        self.program_cycle_journal = root / "program-cycle.sqlite3"
        self.program_runner_liveness_socket = self.manager_run_root / "program-runner-liveness.sock"
        self.history_database = root / "history.sqlite3"
        self.manager_executable = _required_path("MANYFORGE_MANAGER_DAEMON")
        self.package_root = package_root or _required_path("MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT")
        self.manager_package_root = _required_path("MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT")
        self.manager_admission_filename: str | None = None
        self.fixture_metadata: dict | None = None
        if not self.package_root.exists():
            raise RuntimeError(f"retained package root does not exist: {self.package_root}")
        self.cell_id = cell_id
        self.program_path = program_path
        self.composer_fs_roots = composer_fs_roots
        self.freshness_milliseconds = freshness_milliseconds
        self.enable_program_runner = enable_program_runner
        self.port = _free_loopback_port()
        self.program_runner_port = _free_loopback_port()
        self.composer_port = _free_loopback_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.program_runner_url = f"http://127.0.0.1:{self.program_runner_port}"
        self.composer_url = f"http://127.0.0.1:{self.composer_port}"
        self.manager: subprocess.Popen[bytes] | None = None
        self.api: subprocess.Popen[bytes] | None = None
        self.program_runner: subprocess.Popen[bytes] | None = None
        self.composer: subprocess.Popen[bytes] | None = None
        self.log = (root / "process-stack.log").open("wb")
        self._log_drains: list[threading.Thread] = []

    def prepare_native_manager_fixture(
        self,
        *,
        metadata_environment: str = "MANYFORGE_MANAGER_APPLY_METADATA",
        package_environment: str = "MANYFORGE_MANAGER_APPLY_PACKAGE_ROOT",
    ) -> None:
        metadata_path = _required_path(metadata_environment)
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        assert isinstance(metadata, dict)
        assert metadata["cell_id"] == self.cell_id
        self.fixture_metadata = metadata
        self.manager_admission_filename = metadata["admission_filename"]
        assert isinstance(self.manager_admission_filename, str)
        # A copied immutable fixture remains the selected Manager package.
        # The environment path names its source metadata, not a second root.
        _required_path(package_environment)
        self.prepare_native_package(self.manager_admission_filename)

    def prepare_native_package(
        self, admission_filename: str, *, ros2_control: bool = False
    ) -> None:
        """Install one exact retained package's selected process executables.

        The selected product uses the ROS execution host beside Manager.
        The caller's environment must carry the sourced ROS bringup overlay,
        which Manager's children inherit. The legacy keyword is retained for
        existing callers; executable selection always follows the ROS lane.
        """

        self.manager_admission_filename = admission_filename
        self.manager_package_root = self.package_root
        assert (self.manager_package_root / admission_filename).is_file()

        installed_root = self.root / "installed-targets"
        installed_root.mkdir(exist_ok=True)
        installed_sources = {
            "manyforge_manager_daemon": self.manager_executable,
            "manyforge_ros2_control_execution_host": _required_path(
                "MANYFORGE_ROS2_CONTROL_EXECUTION_HOST"
            ),
            "manyforge_roboplan_candidate_worker": _required_path(
                "MANYFORGE_ROBOPLAN_CANDIDATE_WORKER"
            ),
        }
        for installed_name, source in installed_sources.items():
            assert source.is_file() and os.access(source, os.X_OK)
            shutil.copy2(source, installed_root / installed_name)
        self.manager_executable = installed_root / "manyforge_manager_daemon"

    def start_manager(self, *, publication_root: Path | None = None) -> None:
        assert self.manager is None
        manager_publication_root = publication_root or self.publication_root
        manager_publication_root.mkdir(exist_ok=True)
        command = [
            str(self.manager_executable),
            "--install-root",
            str(self.manager_package_root),
            "--activation-root",
            str(self.artifact_root),
            "--manager-state-root",
            str(self.manager_root),
            "--manager-run-root",
            str(self.manager_run_root),
            "--execution-state-root",
            str(self.execution_state_root),
            "--execution-run-root",
            str(self.execution_run_root),
            "--logical-scene-database",
            str(self.scene_database),
            "--program-cycle-journal",
            str(self.program_cycle_journal),
            "--projection-root",
            str(manager_publication_root),
            "--reconciliation-period-ms",
            "20",
        ]
        if self.manager_admission_filename is not None:
            command.extend(
                (
                    "--canonical-empty-admission-filename",
                    self.manager_admission_filename,
                )
            )
        if self.enable_program_runner:
            command.extend(
                (
                    "--program-runner-liveness-socket",
                    str(self.program_runner_liveness_socket),
                )
            )
        # A pipe, as in production (journald): the execution host inherits
        # this stdout and forwards its controller-launch children's output
        # only to a pipe, never to a regular file.
        self.manager = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        assert self.manager.stdout is not None
        drain = threading.Thread(
            target=_drain_into,
            args=(self.manager.stdout, os.dup(self.log.fileno())),
            name="manager-output-drain",
            daemon=True,
        )
        drain.start()
        self._log_drains.append(drain)

    def start_api(self) -> None:
        assert self.api is None
        command = [
            sys.executable,
            "-m",
            "manyforge_headless_api",
            "serve",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.port),
            "--projection-root",
            str(self.publication_root),
            "--install-root",
            str(self.package_root),
            "--artifact-root",
            str(self.artifact_root),
            "--node-catalog-path",
            str(NODE_CATALOG),
            "--node-environment-manifest-path",
            str(NODE_ENVIRONMENT_MANIFEST),
            "--alarm-catalog-path",
            str(CATALOG),
            "--historian-database-path",
            str(self.history_database),
            "--manager-run-root",
            str(self.manager_run_root),
            "--execution-run-root",
            str(self.execution_run_root),
            "--cell-id",
            self.cell_id,
            "--owner-id",
            "headless-api.process-test",
            "--freshness-ms",
            str(self.freshness_milliseconds),
        ]
        if self.manager_admission_filename is not None:
            command.extend(
                (
                    "--canonical-empty-admission-filename",
                    self.manager_admission_filename,
                )
            )
        if self.enable_program_runner:
            command.extend(
                (
                    "--program-runner-url",
                    self.program_runner_url,
                    "--program-runner-timeout-s",
                    "8",
                )
            )
        activation_preparer = os.environ.get("MANYFORGE_ACTIVATION_PREPARER")
        if activation_preparer:
            command.extend(
                (
                    "--activation-preparer",
                    str(_required_path("MANYFORGE_ACTIVATION_PREPARER")),
                    "--activation-preparer-capability-root",
                    str(Path(__file__).parents[2]),
                    "--activation-preparer-activation-root",
                    str(self.artifact_root),
                )
            )
        self.api = subprocess.Popen(
            command,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=os.environ.copy(),
        )

    def start_program_runner(self) -> None:
        assert self.enable_program_runner
        assert self.program_runner is None
        self.program_runner = subprocess.Popen(
            [
                sys.executable,
                "-m",
                "manyforge_program_runner",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.program_runner_port),
                "--headless-url",
                self.base_url,
                "--execution-run-root",
                str(self.execution_run_root),
                "--cycle-journal-path",
                str(self.program_cycle_journal),
                "--manager-liveness-socket",
                str(self.program_runner_liveness_socket),
                "--cell-id",
                self.cell_id,
                "--owner-id",
                "program-runner.process-test",
                "--headless-timeout-s",
                "8",
            ],
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=os.environ.copy(),
        )

    def wait_program_runner(self, timeout_seconds: float = 8.0) -> dict:
        assert self.enable_program_runner
        deadline = time.monotonic() + timeout_seconds
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.program_runner is not None and self.program_runner.poll() is not None:
                raise RuntimeError(
                    "Program runner exited with " f"{self.program_runner.returncode}"
                )
            try:
                with ProgramRunnerHttpClient(
                    self.program_runner_url,
                    timeout_seconds=0.25,
                ) as client:
                    return client.runs(active_only=True, limit=1)
            except Exception as error:
                last_error = error
            time.sleep(0.01)
        raise RuntimeError(f"Program runner did not become available: {last_error}")

    def wait_complete(
        self,
        expected: bool,
        timeout_seconds: float = 8.0,
        *,
        unavailable_section: str | None = None,
    ) -> dict:
        deadline = time.monotonic() + timeout_seconds
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            for process, name in ((self.manager, "Manager"), (self.api, "API")):
                if process is not None and process.poll() is not None:
                    raise RuntimeError(f"{name} exited with {process.returncode}")
            try:
                with HeadlessHttpClient(
                    self.base_url,
                    timeout_seconds=0.25,
                ) as client:
                    runtime = client.runtime()
                unavailable = runtime.get("unavailable_sections")
                section_observed = unavailable_section is None or (
                    isinstance(unavailable, list) and unavailable_section in unavailable
                )
                if runtime.get("complete") is expected and section_observed:
                    return runtime
            except HeadlessClientError as exc:
                last_error = exc
            time.sleep(0.01)
        raise RuntimeError(f"runtime did not reach complete={expected}: {last_error}")

    def wait_manager_source(self, timeout_seconds: float = 8.0) -> dict:
        deadline = time.monotonic() + timeout_seconds
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.api is not None and self.api.poll() is not None:
                raise RuntimeError(f"API exited with {self.api.returncode}")
            try:
                with HeadlessHttpClient(
                    self.base_url,
                    timeout_seconds=0.25,
                ) as client:
                    return client.manager_status()
            except HeadlessClientError as exc:
                last_error = exc
            time.sleep(0.01)
        raise RuntimeError(f"Manager source did not become available: {last_error}")

    def wait_authoring_admission(
        self,
        timeout_seconds: float = 8.0,
        *,
        expected_allowed: bool | None = None,
    ) -> dict:
        deadline = time.monotonic() + timeout_seconds
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            for process, name in ((self.manager, "Manager"), (self.api, "API")):
                if process is not None and process.poll() is not None:
                    raise RuntimeError(f"{name} exited with {process.returncode}")
            try:
                with HeadlessHttpClient(
                    self.base_url,
                    timeout_seconds=0.25,
                ) as client:
                    admission = client.authoring_admission()
                if (
                    expected_allowed is None
                    or admission.get("authoring_allowed") is expected_allowed
                ):
                    return admission
                last_error = RuntimeError(f"last authoring admission was {admission!r}")
            except HeadlessClientError as error:
                last_error = error
            time.sleep(0.01)
        raise RuntimeError(f"authoring admission did not become available: {last_error}")

    def start_composer(self) -> None:
        assert self.composer is None
        command = [
            sys.executable,
            "-m",
            "manyforge_composer",
            "--host",
            "127.0.0.1",
            "--port",
            str(self.composer_port),
            "--headless-api-url",
            self.base_url,
            "--headless-api-timeout-s",
            "8",
        ]
        if self.program_path is not None:
            command.extend(("--program-path", str(self.program_path)))
        environment = os.environ.copy()
        if self.composer_fs_roots is not None:
            environment["MANYFORGE_FS_ROOTS"] = ",".join(
                str(path) for path in self.composer_fs_roots
            )
        self.composer = subprocess.Popen(
            command,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=environment,
        )

    def wait_composer(self, timeout_seconds: float = 8.0) -> dict:
        deadline = time.monotonic() + timeout_seconds
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.composer is not None and self.composer.poll() is not None:
                self.log.flush()
                recent = (self.root / "process-stack.log").read_text(
                    encoding="utf-8", errors="replace"
                ).splitlines()[-20:]
                raise RuntimeError(
                    f"Composer exited with {self.composer.returncode}: "
                    + " | ".join(recent)
                )
            try:
                with HeadlessHttpClient(
                    self.composer_url,
                    timeout_seconds=0.25,
                ) as client:
                    return client.runtime()
            except HeadlessClientError as exc:
                last_error = exc
            time.sleep(0.01)
        raise RuntimeError(f"Composer did not become available: {last_error}")

    def kill_composer(self) -> None:
        assert self.composer is not None
        self.composer.kill()
        self.composer.wait(timeout=5)
        self.composer = None

    def kill_api(self) -> None:
        assert self.api is not None
        self.api.kill()
        self.api.wait(timeout=5)
        self.api = None

    def kill_program_runner(self) -> None:
        assert self.program_runner is not None
        self.program_runner.kill()
        self.program_runner.wait(timeout=5)
        self.program_runner = None

    def kill_manager(self) -> None:
        assert self.manager is not None
        self.manager.kill()
        self.manager.wait(timeout=5)
        self.manager = None

    def private_shutdown_manager(self) -> None:
        if self.manager is None or self.manager.poll() is not None:
            self.manager = None
            return
        request = b'{"api_version":"manyforge.manager_command.v1",' b'"command":"shutdown_runtime"}'
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(1.0)
                client.connect(str(self.manager_run_root / "manager-command.sock"))
                client.sendall(request)
                response = client.recv(16 * 1024)
                if b'"ok":true' not in response:
                    raise RuntimeError("Manager shutdown was rejected")
            self.manager.wait(timeout=5)
        except (OSError, RuntimeError, subprocess.TimeoutExpired):
            # Cleanup must still terminate a failed or unresponsive Manager,
            # without replacing the process-boundary assertion that led here.
            pass
        finally:
            if self.manager.poll() is None:
                self.manager.terminate()
                try:
                    self.manager.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.manager.kill()
                    self.manager.wait(timeout=5)
            self.manager = None

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
        try:
            if self.composer is not None:
                self._terminate(self.composer)
            self.composer = None
        finally:
            try:
                if self.program_runner is not None:
                    self._terminate(self.program_runner)
                self.program_runner = None
            finally:
                try:
                    self.private_shutdown_manager()
                finally:
                    if self.api is not None:
                        self._terminate(self.api)
                    self.api = None
                    # The pipe ends when Manager and every inheriting
                    # descendant have exited; a lingering one cannot block.
                    for drain in self._log_drains:
                        drain.join(timeout=10.0)
                    self.log.close()


@contextmanager
def _stack(root: Path) -> Iterator[ProcessStack]:
    stack = ProcessStack(root)
    try:
        stack.start_manager()
        stack.start_api()
        stack.wait_complete(True)
        yield stack
    finally:
        stack.close()


@contextmanager
def _program_surface_stack(root: Path) -> Iterator[ProcessStack]:
    stack = ProcessStack(root, enable_program_runner=True)
    try:
        stack.start_manager()
        stack.start_program_runner()
        stack.start_api()
        stack.wait_complete(True)
        stack.wait_program_runner()
        yield stack
    finally:
        stack.close()


@contextmanager
def _assistant_stack(
    root: Path,
    *,
    package_root: Path | None = None,
    metadata_environment: str = "MANYFORGE_ASSISTANT_NATIVE_METADATA",
    package_environment: str = "MANYFORGE_ASSISTANT_NATIVE_PACKAGE_ROOT",
    startup_scenario: str | None = None,
) -> Iterator[ProcessStack]:
    selected_package_root = package_root or _required_path(package_environment)
    cell_id = json.loads(_required_path(metadata_environment).read_text("utf-8"))["cell_id"]
    retained_programs = sorted((selected_package_root / "programs" / "sha256").glob("*.json"))
    assert len(retained_programs) == 1
    stack = ProcessStack(
        root,
        package_root=selected_package_root,
        cell_id=cell_id,
        program_path=retained_programs[0],
        freshness_milliseconds=120000,
        enable_program_runner=True,
    )
    try:
        stack.prepare_native_manager_fixture(
            metadata_environment=metadata_environment,
            package_environment=package_environment,
        )
        # The synthetic process fixture was produced into one package tree,
        # whereas Headless owns Program and binding repositories under its
        # separate artifact root. Install those immutable fixture values into
        # that repository before any owner starts.
        # The activation root holds each prepared value's complete closure,
        # as the native preparer publishes it.
        for repository in ("programs", "program-bindings", "prepared-activations",
                           "catalog-artifacts", "raw-artifacts", "retained-bundles"):
            source = selected_package_root / repository
            if source.is_dir():
                shutil.copytree(source, stack.artifact_root / repository)
        assert stack.manager_package_root == stack.package_root
        stack.start_manager()
        stack.start_program_runner()
        stack.start_api()
        initial = stack.wait_manager_source()
        stack.wait_program_runner()
        initial_admission = stack.wait_authoring_admission(expected_allowed=True)
        assert stack.fixture_metadata is not None
        metadata = stack.fixture_metadata
        environment_manifests = sorted(
            (selected_package_root / "build-manifests" / "node_environment").glob("*.json")
        )
        assert len(environment_manifests) == 1
        environment_digest = json.loads(
            environment_manifests[0].read_text(encoding="utf-8")
        )["environment_digest"]
        assert initial["status"]["active"]["retained_bundle_id"] == (
            metadata["predecessor_retained_bundle_id"]
        )
        with HeadlessHttpClient(stack.base_url, timeout_seconds=20.0) as client:
            fixture_program = json.loads(stack.program_path.read_text(encoding="utf-8"))
            # The fixture generator published real, reverified prepared
            # activations for each Scenario it serves; Manager admits only
            # stored values whose summary equals the request exactly.
            scenario_id = startup_scenario or "assistant_native_process_fixture"
            prepared_activation = dict(metadata["prepared_activations"][scenario_id])
            assert prepared_activation["program_id"] == fixture_program["program_id"]
            assert prepared_activation["node_environment_digest"] == environment_digest
            origin = PreparedScenarioOrigin.model_validate(prepared_activation["origin"])
            assert origin.scenario_id == scenario_id
            metadata["source_document_digest"] = origin.source_document_digest
            apply_body = {
                "request_id": "assistant-native.process.apply",
                "request_owner_epoch": initial["source"]["source_owner_epoch"],
                "source_label": "assistant_native_process_test",
                "correlation_id": "assistant-native.process.apply",
                "deployment_artifact_id": metadata["physical_deployment_artifact_id"],
                "deployment_revision": metadata["physical_deployment_revision"],
                "expected_current": {
                    "graph_revision": metadata["predecessor_graph_revision"],
                    "retained_bundle_id": metadata["predecessor_retained_bundle_id"],
                },
                "reject_if_affected_busy": True,
                "expected_admission_generation": initial_admission["admission_generation"],
                "prepared_activation": prepared_activation,
            }
            # This legacy process fixture supplies a synthetic, exact retained
            # Apply identity rather than using the authored-preparation owner.
            # Request its preview from Manager's private boundary; the public
            # Headless preview correctly requires a registered preparation.
            preview = ManagerCommandClient(stack.manager_run_root).preview_deployment_candidate(
                ManagerDeploymentCandidatePreviewRequest.model_validate({
                    "deployment_artifact_id": apply_body["deployment_artifact_id"],
                    "deployment_revision": apply_body["deployment_revision"],
                    "candidate_retained_bundle_id": prepared_activation["retained_bundle_id"],
                    "candidate_graph_revision": prepared_activation["graph_revision"],
                    "prepared_activation_id": prepared_activation["prepared_activation_id"],
                    "expected_current": apply_body["expected_current"],
                    "expected_admission_generation": apply_body["expected_admission_generation"],
                    "request_owner_epoch": apply_body["request_owner_epoch"],
                })
            )
            apply_result = client.manager_apply(
                {
                    **apply_body,
                    "candidate_receipt": preview["candidate_receipt"],
                    # The test caller explicitly accepts the previewed impact.
                    "accepted_impact_digest": preview["candidate_receipt"][
                        "impact_digest"
                    ],
                }
            )
            assert apply_result["ok"] is True
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            active = stack.wait_manager_source()["status"]["active"]
            if active["retained_bundle_id"] == metadata["physical_candidate_retained_bundle_id"]:
                break
            time.sleep(0.02)
        else:
            stack.log.flush()
            recent = (stack.root / "process-stack.log").read_text(
                encoding="utf-8", errors="replace"
            ).splitlines()[-20:]
            raise RuntimeError(
                "assistant-native physical Apply did not converge: " + " | ".join(recent)
            )
        admission = stack.wait_authoring_admission(expected_allowed=True)
        assert admission["authoring_allowed"] is True
        assert admission["admission_generation"] > 0
        assert (stack.execution_run_root / "execution-command.sock").is_socket()
        assert (stack.execution_run_root / "execution-observation.sock").is_socket()
        stack.start_composer()
        stack.wait_composer()
        yield stack
    finally:
        stack.close()


def _composer_snapshot_stream(stack: ProcessStack, *, client_id: str):
    return composer_snapshot_stream(stack.composer_url, client_id=f"process-test.{client_id}")


def _composer_fence(
    snapshot: dict,
    *,
    request_id: str,
) -> dict:
    workspace = snapshot["authoring_workspace"]
    admission = snapshot["authoring_admission"]
    return {
        "draftId": workspace["program_draft_id"],
        "expectedDraftRevision": workspace["program_draft_revision"],
        "expectedWorkspaceEpoch": workspace["workspace_epoch"],
        "expectedWorkspaceRevision": workspace["workspace_revision"],
        "expectedAdmissionGeneration": admission["admission_generation"],
        "requestOwnerEpoch": "composer.process.epoch.1",
        "requestId": request_id,
    }


def _matching_composer_snapshot(next_snapshot: Callable[[], dict], draft: dict) -> dict:
    for _ in range(32):
        snapshot = next_snapshot()
        workspace = snapshot.get("authoring_workspace")
        if isinstance(workspace, dict) and (
            workspace.get("program_draft_id") == draft["draft_id"]
            and workspace.get("program_draft_revision") == draft["draft_revision"]
        ):
            return snapshot
    raise RuntimeError("Composer did not publish the exact current Draft snapshot")


def _matching_assistant_context(stack: ProcessStack) -> tuple[dict, dict]:
    # One context read projects node environment, source model and assistant
    # mode from one runtime snapshot, as Composer reads it for callbacks.
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        with HeadlessHttpClient(stack.base_url) as headless:
            try:
                context = headless.assistant_mode_context("composer-assistant")
            except HeadlessClientError as error:
                if error.code not in {
                    "assistant_mode_projection_invalid",
                    "assistant_mode_projection_unavailable",
                    "node_environment_projection_invalid",
                }:
                    raise
                time.sleep(0.01)
                continue
        return context, context["assistant_mode"]
    raise RuntimeError("Headless did not publish a coherent assistant context")


def _unavailable_candidate_preview(
    client: HeadlessHttpClient,
    manager_run_root: Path,
    owner_epoch: str,
    admission_generation: int,
) -> dict:
    active = client.manager_status()["status"]["active"]
    # The private preview itself must reject a nonexistent candidate before
    # any public Apply can obtain a candidate receipt.
    return ManagerCommandClient(manager_run_root).preview_deployment_candidate(
        ManagerDeploymentCandidatePreviewRequest.model_validate({
            "deployment_artifact_id": "candidate",
            "deployment_revision": "sha256:" + "a" * 64,
            "candidate_retained_bundle_id": "sha256:" + "b" * 64,
            "candidate_graph_revision": "sha256:" + "c" * 64,
            "prepared_activation_id": "sha256:" + "f" * 64,
            "expected_current": {
                "graph_revision": active["graph_revision"],
                "retained_bundle_id": active["retained_bundle_id"],
            },
            "expected_admission_generation": admission_generation,
            "request_owner_epoch": owner_epoch,
        })
    )


def _assistant_callback_body(
    *,
    draft: dict,
    workspace: dict,
    node_environment: dict,
    source_model: dict,
    assistant_mode: dict,
    scene_resource_id: str,
    request_id: str,
    admission_generation: int,
) -> dict:
    tool = next(
        row for row in assistant_mode["tools"] if row["tool_id"] == "scene_draft_add_object"
    )
    return {
        "assistantMode": "composer-assistant",
        "retainedBundleId": node_environment["retained_bundle_id"],
        "graphRevision": node_environment["graph_revision"],
        "nodeEnvironmentDigest": node_environment["environment_digest"],
        "nodeEnvironmentProjectionDigest": node_environment["source"]["source_content_digest"],
        "sourceModelProjectionDigest": source_model["source"]["source_content_digest"],
        "assistantProjectionDigest": assistant_mode["assistant_projection_digest"],
        "assistantModeProjectionDigest": assistant_mode["source"]["source_content_digest"],
        "catalogHash": assistant_mode["catalog_hash"],
        "requestedTools": [],
        "fieldPolicy": {},
        "inputSchemaHash": tool["input_schema_hash"],
        "draftId": draft["draft_id"],
        "expectedDraftRevision": draft["draft_revision"],
        "expectedWorkspaceEpoch": workspace["workspaceEpoch"],
        "expectedWorkspaceRevision": workspace["workspaceRevision"],
        "expectedAdmissionGeneration": admission_generation,
        "requestOwnerEpoch": "composer.process.epoch.1",
        "requestId": request_id,
        "sourceLabel": "assistant_process_test",
        "correlationId": "correlation.assistant.1",
        "arguments": {
            "scene_resource": {
                "scene_resource_id": scene_resource_id,
                "shape": {
                    "type": "box",
                    "box_dimensions_m": [0.1, 0.2, 0.3],
                },
                "pose": {
                    "frame_ref": {"kind": "cell_frame"},
                    "position_m": [0.0, 0.0, 0.0],
                    "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
                },
            }
        },
    }


def _current_authoring_generation(client: HeadlessHttpClient) -> int:
    admission = client.authoring_admission()
    generation = admission.get("admission_generation")
    assert admission.get("authoring_allowed") is True
    assert type(generation) is int and generation >= 1
    return generation


def _plain_package_files(root: Path) -> dict[str, tuple[bytes, int, int]]:
    result: dict[str, tuple[bytes, int, int]] = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and not path.is_symlink():
            stat = path.stat()
            result[str(path.relative_to(root))] = (
                path.read_bytes(),
                stat.st_size,
                stat.st_mtime_ns,
            )
    return result


@contextmanager
def _read_only_tree(root: Path) -> Iterator[None]:
    modes: dict[Path, int] = {}
    paths = [root, *root.rglob("*")]
    for path in paths:
        if path.is_symlink():
            continue
        modes[path] = path.stat().st_mode & 0o777
        path.chmod(modes[path] & ~0o222)
    try:
        yield
    finally:
        for path, mode in reversed(list(modes.items())):
            if path.exists():
                path.chmod(mode)


def _owner_identity(runtime: dict, alarms: dict) -> tuple[object, ...]:
    return (
        runtime["projection_owner_epoch"],
        runtime["projection_generation"],
        alarms["owner_epoch"],
        alarms["generation"],
        alarms["state_revision"],
    )


def test_real_assistant_callback_uses_headless_source_facts_and_only_edits_draft(
    tmp_path: Path,
) -> None:
    with _assistant_stack(tmp_path) as stack:
        with (
            HeadlessHttpClient(stack.composer_url) as composer,
            HeadlessHttpClient(stack.base_url) as headless,
        ):
            source_context, assistant_mode = _matching_assistant_context(stack)
            node_environment = source_context["node_environment"]
            source_model = source_context["source_model"]
            draft = composer.request("GET", "/api/program/draft")
            manager_before = composer.manager_status()["status"]
            runtime_before = composer.runtime()
            package_before = _plain_package_files(stack.package_root)
            assert stack.fixture_metadata is not None
            active_bundle = stack.fixture_metadata["physical_candidate_retained_bundle_id"]
            active_graph = stack.fixture_metadata["physical_graph_revision"]

            # The UR10e + Robotiq 2F-140 example cell: one model per physical
            # component; the assistant's AI model has its model-free endpoint.
            assert {row["model_instance_id"] for row in source_model["occurrences"]} == {
                "ur10e_robot.model", "robotiq_2f_140.model"
            }
            assert {row["component_id"] for row in source_model["targets"]} == {
                "ur10e_robot", "robotiq_2f_140", "assistant_model"
            }
            assert node_environment["retained_bundle_id"] == active_bundle
            assert node_environment["graph_revision"] == active_graph
            assert source_model["retained_bundle_id"] == active_bundle
            assert source_model["graph_revision"] == active_graph
            assert assistant_mode["retained_bundle_id"] == active_bundle
            assert assistant_mode["graph_revision"] == active_graph
            assert manager_before["active"]["retained_bundle_id"] == active_bundle
            assert manager_before["active"]["graph_revision"] == active_graph
            # The example's `assistant_scene_authoring` profile: this test uses
            # these two of its tools.
            assert {"program_read", "scene_draft_add_object"} <= {
                row["tool_id"] for row in assistant_mode["tools"]
            }

            # A refreshed owner projection or admission generation changes the
            # exact payload, so every new attempt needs a new idempotency key.
            for attempt in range(8):
                source_context, assistant_mode = _matching_assistant_context(stack)
                node_environment = source_context["node_environment"]
                source_model = source_context["source_model"]
                accepted_body = _assistant_callback_body(
                    draft=draft,
                    workspace=composer.request("GET", "/api/program/workspace")["authoringWorkspace"],
                    node_environment=node_environment,
                    source_model=source_model,
                    assistant_mode=assistant_mode,
                    scene_resource_id="assistant_workpiece",
                    request_id=f"assistant.process.accepted.{attempt}",
                    admission_generation=_current_authoring_generation(headless),
                )
                assert {
                    "clientId",
                    "snapshotReceiverEpoch",
                    "snapshotSequence",
                }.isdisjoint(accepted_body)
                try:
                    accepted = composer.request(
                        "POST",
                        "/api/assistant/bridge/tools/scene_draft_add_object",
                        body=accepted_body,
                    )
                    break
                except HeadlessClientError as error:
                    if (
                        error.code
                        not in {
                            "assistant_callback_context_stale",
                            "assistant_mode_projection_invalid",
                            "node_environment_projection_invalid",
                            "source_model_projection_invalid",
                            "stale_generation",
                        }
                        or attempt == 7
                    ):
                        raise
            assert accepted["success"] is True
            assert accepted["draftMutated"] is True
            # A Draft edit takes no Kernel claim: the generation is echoed.
            assert (
                accepted["nextAdmissionGeneration"] == accepted_body["expectedAdmissionGeneration"]
            )

            after_accepted = composer.request("GET", "/api/program/draft")
            assert after_accepted["draft_revision"] == draft["draft_revision"] + 1
            assert after_accepted["saved"] is False
            assert after_accepted["applied"] is False
            assert "assistant_workpiece" in {
                row["scene_resource_id"] for row in after_accepted["program"]["scene"]
            }
            for attempt in range(8):
                stale_context, stale_mode = _matching_assistant_context(stack)
                stale_identity = _assistant_callback_body(
                    draft=after_accepted,
                    workspace=composer.request("GET", "/api/program/workspace")["authoringWorkspace"],
                    node_environment=stale_context["node_environment"],
                    source_model=stale_context["source_model"],
                    assistant_mode=stale_mode,
                    scene_resource_id="stale_identity_object",
                    request_id=f"assistant.process.stale-identity.{attempt}",
                    admission_generation=_current_authoring_generation(headless),
                )
                stale_identity["catalogHash"] = "sha256:" + "0" * 64
                with pytest.raises(HeadlessClientError) as stale:
                    composer.request(
                        "POST",
                        "/api/assistant/bridge/tools/scene_draft_add_object",
                        body=stale_identity,
                    )
                if stale.value.code == "assistant_callback_context_stale":
                    break
                if (
                    stale.value.code
                    not in {
                        "assistant_mode_projection_invalid",
                        "node_environment_projection_invalid",
                        "source_model_projection_invalid",
                        "stale_generation",
                    }
                    or attempt == 7
                ):
                    raise stale.value
            assert stale.value.code == "assistant_callback_context_stale"

            for attempt in range(8):
                invalid_context, invalid_mode = _matching_assistant_context(stack)
                invalid_edit = _assistant_callback_body(
                    draft=after_accepted,
                    workspace=composer.request("GET", "/api/program/workspace")["authoringWorkspace"],
                    node_environment=invalid_context["node_environment"],
                    source_model=invalid_context["source_model"],
                    assistant_mode=invalid_mode,
                    scene_resource_id="invalid_edit_object",
                    request_id=f"assistant.process.invalid-edit.{attempt}",
                    admission_generation=_current_authoring_generation(headless),
                )
                resource = invalid_edit["arguments"]["scene_resource"]
                resource["objectId"] = resource.pop("scene_resource_id")
                try:
                    rejected = composer.request(
                        "POST",
                        "/api/assistant/bridge/tools/scene_draft_add_object",
                        body=invalid_edit,
                    )
                    break
                except HeadlessClientError as error:
                    if (
                        error.code
                        not in {
                            "assistant_mode_projection_invalid",
                            "node_environment_projection_invalid",
                            "source_model_projection_invalid",
                            "stale_generation",
                        }
                        or attempt == 7
                    ):
                        raise
            assert rejected["success"] is False
            assert rejected["draftMutated"] is False
            assert (
                rejected["nextAdmissionGeneration"] == invalid_edit["expectedAdmissionGeneration"]
            )
            assert rejected["result"]["kind"] == "validation_error"
            assert composer.request("GET", "/api/program/draft") == after_accepted

            callback_latencies_s: list[float] = []
            for index in range(12):
                for attempt in range(8):
                    read_context, read_mode = _matching_assistant_context(stack)
                    program_read = next(
                        row for row in read_mode["tools"] if row["tool_id"] == "program_read"
                    )
                    read_body = _assistant_callback_body(
                        draft=after_accepted,
                        workspace=composer.request("GET", "/api/program/workspace")["authoringWorkspace"],
                        node_environment=read_context["node_environment"],
                        source_model=read_context["source_model"],
                        assistant_mode=read_mode,
                        scene_resource_id="unused_for_program_read",
                        request_id=f"assistant.process.read.{index}.{attempt}",
                        admission_generation=_current_authoring_generation(headless),
                    )
                    read_body["inputSchemaHash"] = program_read["input_schema_hash"]
                    read_body["arguments"] = {}
                    started = time.perf_counter()
                    try:
                        read_result = composer.request(
                            "POST",
                            "/api/assistant/bridge/tools/program_read",
                            body=read_body,
                        )
                        callback_latencies_s.append(time.perf_counter() - started)
                        break
                    except HeadlessClientError as error:
                        if (
                            error.code
                            not in {
                                "assistant_callback_context_stale",
                                "assistant_mode_projection_invalid",
                                "node_environment_projection_invalid",
                                "source_model_projection_invalid",
                                "stale_generation",
                            }
                            or attempt == 7
                        ):
                            raise
                assert read_result["success"] is True
                assert read_result["draftMutated"] is False
                assert (
                    read_result["nextAdmissionGeneration"]
                    == read_body["expectedAdmissionGeneration"]
                )
            assert max(callback_latencies_s) < 0.5, callback_latencies_s

        stack.kill_composer()
        stack.start_composer()
        stack.wait_composer()
        with (
            HeadlessHttpClient(stack.composer_url) as replacement,
            HeadlessHttpClient(stack.base_url) as headless,
        ):
            replacement_draft = replacement.request("GET", "/api/program/draft")
            assert replacement_draft["draft_id"] != after_accepted["draft_id"]
            assert "assistant_workpiece" not in {
                row["scene_resource_id"]
                for row in replacement_draft["program"].get("scene", [])
            }

            def invoke(body: dict) -> tuple[str, str]:
                try:
                    with HeadlessHttpClient(stack.composer_url) as client:
                        result = client.request(
                            "POST",
                            "/api/assistant/bridge/tools/scene_draft_add_object",
                            body=body,
                        )
                    return ("accepted", str(result["result"]["draftRevision"]))
                except HeadlessClientError as error:
                    return ("rejected", error.code)

            replacement_context, replacement_mode = _matching_assistant_context(stack)
            concurrent_generation = _current_authoring_generation(headless)
            concurrent_bodies = [
                _assistant_callback_body(
                    draft=replacement_draft,
                    workspace=replacement.request("GET", "/api/program/workspace")["authoringWorkspace"],
                    node_environment=replacement_context["node_environment"],
                    source_model=replacement_context["source_model"],
                    assistant_mode=replacement_mode,
                    scene_resource_id=f"concurrent_object_{index}",
                    request_id=f"assistant.process.concurrent.{index}",
                    admission_generation=concurrent_generation,
                )
                for index in range(2)
            ]
            with ThreadPoolExecutor(max_workers=2) as executor:
                concurrent_results = list(executor.map(invoke, concurrent_bodies))
        accepted_count = sum(kind == "accepted" for kind, _detail in concurrent_results)
        assert accepted_count <= 1
        assert all(
            detail
            in {
                "assistant_callback_context_stale",
                "assistant_mode_projection_invalid",
                "authoring_not_ready",
                "composer_authoring_recovery_blocked",
                "composer_draft_revision_stale",
                "node_environment_projection_invalid",
                "source_model_projection_invalid",
                "stale_generation",
            }
            for kind, detail in concurrent_results
            if kind == "rejected"
        ), concurrent_results
        assert (
            len([detail for kind, detail in concurrent_results if kind == "rejected"])
            == 2 - accepted_count
        )

        with HeadlessHttpClient(stack.composer_url) as composer:
            final_draft = composer.request("GET", "/api/program/draft")
            concurrent_ids = {
                row["scene_resource_id"]
                for row in final_draft["program"].get("scene", [])
                if row["scene_resource_id"].startswith("concurrent_object_")
            }
            assert len(concurrent_ids) == accepted_count
            assert final_draft["draft_revision"] == (
                replacement_draft["draft_revision"] + accepted_count
            )
            assert final_draft["saved"] is False
            assert final_draft["applied"] is False
            assert composer.manager_status()["status"]["active"] == manager_before["active"]
            runtime_after = composer.runtime()
            assert (
                runtime_after["active_retained_bundle_id"]
                == runtime_before["active_retained_bundle_id"]
            )
            assert runtime_after["active_graph_revision"] == runtime_before["active_graph_revision"]
            assert _plain_package_files(stack.package_root) == package_before


def test_program_runner_headless_and_composer_are_independent_processes(
    tmp_path: Path,
) -> None:
    with _program_surface_stack(tmp_path) as stack:
        with HeadlessHttpClient(stack.base_url) as headless:
            manager_before = headless.manager_status()["status"]["active"]
            initial = headless.program_runs(active_only=True, limit=1)

        stack.start_composer()
        stack.wait_composer()
        with HeadlessHttpClient(stack.composer_url) as composer:
            through_composer = composer.program_runs(active_only=True, limit=1)
        assert through_composer == initial

        stack.kill_composer()
        with HeadlessHttpClient(stack.base_url) as headless:
            after_composer_loss = headless.program_runs(
                active_only=True,
                limit=1,
            )
            manager_after = headless.manager_status()["status"]["active"]
        assert after_composer_loss == initial
        assert manager_after == manager_before

        stack.kill_program_runner()
        with HeadlessHttpClient(stack.base_url) as headless:
            with pytest.raises(HeadlessClientError) as unavailable:
                headless.program_runs(active_only=True, limit=1)
        assert unavailable.value.code == "program_runner_unavailable"

        stack.start_program_runner()
        replacement = stack.wait_program_runner()
        with HeadlessHttpClient(stack.base_url) as headless:
            forwarded_replacement = headless.program_runs(
                active_only=True,
                limit=1,
            )
        assert replacement["owner_epoch"] != initial["owner_epoch"]
        assert forwarded_replacement == replacement


def test_manager_api_cli_and_replacements_preserve_headless_owners(
    tmp_path: Path,
) -> None:
    with _stack(tmp_path) as stack:
        with HeadlessHttpClient(stack.base_url) as client:
            runtime_before_composer = client.runtime()
            alarms_before_composer = client.alarms(limit=128)
            owner_identity = _owner_identity(
                runtime_before_composer,
                alarms_before_composer,
            )
            status = client.manager_status()
            first_epoch = status["source"]["source_owner_epoch"]
            assert status["status"]["manager_ready"] is True
            environment = client.node_environment()
            assert environment["retained_bundle_id"] == (
                status["status"]["active"]["retained_bundle_id"]
            )
            assert environment["graph_revision"] == (status["status"]["active"]["graph_revision"])
            assert environment["manifest"]["environment_digest"] == (
                environment["environment_digest"]
            )
            assert environment["catalog"]["catalog_digest"] == (environment["catalog_digest"])
            assert environment["availability"]["execution_available"] is False
            assert client.manager_graph()["cell_graph"]["cell_id"] == ("c1_build_authority_cell")
            assert client.manager_diff()["converged"] is True
            assert client.scene()["scene_runtime"]["payload"]["state"]["state"] == ("open")

            unavailable = _unavailable_candidate_preview(
                client, stack.manager_run_root, first_epoch, 1
            )
            assert unavailable["ok"] is False
            assert unavailable["error"]["code"] == "deployment_candidate_rejected"

        stack.start_composer()
        assert stack.wait_composer()["projection_owner_epoch"] == owner_identity[0]
        with HeadlessHttpClient(stack.composer_url) as composer:
            assert composer.manager_graph()["cell_graph"]["cell_id"] == ("c1_build_authority_cell")
            assert (
                _owner_identity(
                    composer.runtime(),
                    composer.alarms(limit=128),
                )
                == owner_identity
            )
            assert composer.node_environment()["environment_digest"] == (
                environment["environment_digest"]
            )
            assert (
                composer.request("GET", "/api/catalog")["environmentDigest"]
                == environment["environment_digest"]
            )
            assert (
                composer.request("GET", "/api/nodes/environment")["active"]["retainedBundleId"]
                == environment["retained_bundle_id"]
            )

        stack.kill_composer()
        with HeadlessHttpClient(stack.base_url) as client:
            assert (
                _owner_identity(
                    client.runtime(),
                    client.alarms(limit=128),
                )
                == owner_identity
            )

        stack.start_composer()
        stack.wait_composer()
        with HeadlessHttpClient(stack.composer_url) as replacement_composer:
            assert (
                _owner_identity(
                    replacement_composer.runtime(),
                    replacement_composer.alarms(limit=128),
                )
                == owner_identity
            )

        cli_reads = {
            "health": "manyforge.headless_health.v1",
            "manager-graph": "manyforge.manager_graph_projection.v1",
            "node-environment": "manyforge.node_environment_projection.v2",
            "source-model": "manyforge.source_model_projection.v3",
            "scene": "manyforge.scene_projection.v1",
            "alarms": "manyforge.operator_alarm_snapshot.v2",
            "history": "manyforge.historian_summary_page.v2",
        }
        cli_payloads: dict[str, dict] = {}
        for cli_operation, expected_schema in cli_reads.items():
            command = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "manyforge_headless_api",
                    cli_operation,
                    "--url",
                    stack.base_url,
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=5,
                env=os.environ.copy(),
            )
            assert command.returncode == 0, command.stderr
            payload = json.loads(command.stdout)
            assert payload["schema_version"] == expected_schema
            cli_payloads[cli_operation] = payload
        assert cli_payloads["manager-graph"]["graph_revision"]
        assert cli_payloads["node-environment"]["environment_digest"] == (
            environment["environment_digest"]
        )

        stack.kill_manager()
        stale_runtime = stack.wait_complete(False, unavailable_section="manager")
        assert "manager" in stale_runtime["unavailable_sections"]
        with HeadlessHttpClient(stack.composer_url) as composer:
            assert composer.runtime()["complete"] is False
            with pytest.raises(HeadlessClientError) as unavailable_environment:
                composer.request("GET", "/api/catalog")
            assert unavailable_environment.value.code == ("node_environment_projection_unavailable")

        stack.start_manager()
        stack.wait_complete(True)
        with HeadlessHttpClient(stack.base_url) as client:
            replacement = client.manager_status()
            replacement_epoch = replacement["source"]["source_owner_epoch"]
            assert replacement_epoch != first_epoch

            stale = _unavailable_candidate_preview(
                client, stack.manager_run_root, first_epoch, 1
            )
            assert stale["ok"] is False
            assert stale["error"]["code"] == "stale_request_owner_epoch"

            unavailable = _unavailable_candidate_preview(
                client,
                stack.manager_run_root,
                replacement_epoch,
                1,
            )
            assert unavailable["ok"] is False
            assert unavailable["error"]["code"] == "deployment_candidate_rejected"


def test_manager_robotiq_position_cell_has_no_motion_target_and_refuses_disabled_command() -> None:
    from tempfile import TemporaryDirectory
    from typing import Any
    import math
    import sqlite3

    from manyforge_headless_api.cell_admission import CellAdmissionClient
    from manyforge_headless_api.execution import ExecutionCommandClient
    from manyforge_headless_api.models import OperationIntentRequest, StopIntentRequest
    from manyforge_headless_api.process_tests.test_managed_execution_path import (
        TERMINAL_STATES, _direct_control_gripper_operation,
        _status_target, _status_observation,
    )

    package = _required_path("MANYFORGE_ROBOTIQ_POSITION_PACKAGE_ROOT")
    metadata = json.loads(_required_path("MANYFORGE_ROBOTIQ_POSITION_METADATA").read_text("utf-8"))
    bundle = json.loads((package / "retained-bundles" / "sha256" /
        (metadata["physical_candidate_retained_bundle_id"].removeprefix("sha256:") +
         ".json")).read_text("utf-8"))
    assert bundle["retained_bundle_id"] == metadata["physical_candidate_retained_bundle_id"]
    graph = bundle["cell_graph"]
    assert graph["graph_revision"] == metadata["physical_graph_revision"]
    assert len(graph["controllable_components"]) == 1
    bindings = {row["capability_id"]: row for row in graph["control_bindings"]}
    assert len(bindings) == len(graph["control_bindings"])
    assert set(bindings) == {"tool.gripper.command", "tool.gripper.feedback"}
    command_binding = bindings["tool.gripper.command"]
    feedback_binding = bindings["tool.gripper.feedback"]
    alias_identity_fields = {"control_binding_id", "control_binding_revision",
                             "capability_id", "capability_definition_revision"}
    assert {key: value for key, value in command_binding.items()
            if key not in alias_identity_fields} == {
        key: value for key, value in feedback_binding.items()
        if key not in alias_identity_fields
    }
    assert len(command_binding["resource_bindings"]) == 1
    component_id = command_binding["controllable_component_id"]
    expected_domain_ids = sorted({row["control_domain_id"] for row in bindings.values()})
    assert expected_domain_ids == sorted(row["control_domain_id"] for row in graph["control_domains"])
    assert len(graph["runtime_units"]) == 1
    configuration = graph["runtime_units"][0]["configuration"]
    # The model's `lanes.mock` row travels verbatim as `lane_row`; the tool's
    # launched description and gripper joint are compiled into the row.
    [controller_profile] = configuration["controller_profiles"]
    assert "<robot" in controller_profile["description"]
    assert controller_profile["gripper_joint_name"]
    assert {key: value for key, value in controller_profile.items()
            if key not in {"lane_row", "description", "gripper_joint_name"}} == {
        "controllable_component_id": component_id, "frame_prefix": "", "lane": "mock",
        "model_family_id": controller_profile["model_family_id"],
    }
    assert configuration["initial_joint_positions_rad"] == []
    assert configuration["motion_target_limits"] == []
    assert len(configuration["gripper_target_configurations"]) == 1
    authored_target = configuration["gripper_target_configurations"][0]
    assert authored_target["controllable_component_id"] == component_id
    assert authored_target["command_target_id"] == command_binding["command_target_id"]
    assert authored_target["actuation_kind"] == "position_commanded"
    assert authored_target["output_mode"] == "commanded"
    assert [(row["stream_id"], row["budget_ns"])
            for row in configuration["component_streams"]
            if row["controllable_component_id"] == component_id] == [
        ("gripper_feedback", 1000000000),
        ("measured_joints", 250000000),
    ]

    def position_target(snapshot: dict[str, Any]) -> dict[str, Any]:
        assert snapshot["control_state"]["runtime_scale_control"] is None
        assert snapshot["identity"]["retained_bundle_id"] == metadata["physical_candidate_retained_bundle_id"]
        assert snapshot["identity"]["graph_revision"] == metadata["physical_graph_revision"]
        components = snapshot["targets"]["components"]
        assert len(components) == 1
        component = components[0]
        assert component["controllable_component_id"] == command_binding["controllable_component_id"]
        rows = component["targets"]
        targets = {row["operation_capability_id"]: row for row in rows}
        assert len(targets) == len(rows)
        assert set(targets) == {"tool.gripper.command", "tool.gripper.feedback"}
        target = targets["tool.gripper.command"]
        feedback = targets["tool.gripper.feedback"]
        assert target["presentation"] == "gripper" and target["command"] is not None
        assert feedback["presentation"] == "unsupported" and feedback["command"] is None
        assert target["command_target_id"] == feedback["command_target_id"] == (
            command_binding["command_target_id"]
        )
        assert _status_target(snapshot, presentation="gripper") == (component, target)
        # Raw target inventory retains the feedback alias; the sole numeric
        # observation still belongs to the actual direct command channel.
        assert [(row["controllable_component_id"], row["command_target_id"],
                 row["operation_capability_id"])
                for row in snapshot["control_state"]["observed_targets"]] == [
            (component["controllable_component_id"], target["command_target_id"],
             "tool.gripper.command")
        ]
        descriptor = target["command"]["target_descriptor"]
        resource = target["command"]["resource"]
        path = descriptor["contracts"]["control_path"]
        assert all(path[key] == command_binding[key] for key in path)
        assert descriptor["schema_version"] == (
            "manyforge.manual_control_gripper_descriptor.v5"
        )
        assert descriptor["target"] == {
            "component_id": component["controllable_component_id"],
            "command_target_id": target["command_target_id"],
        }
        assert descriptor["actuation_kind"] == "position_commanded"
        assert descriptor["output_mode"] == "commanded"
        assert descriptor["completion_policy"] == "goal_fenced_target_reached"
        assert descriptor["grasp_confirmation_policy"] == "binding_feedback_required"
        assert descriptor["command_duration_s"] == pytest.approx(
            authored_target["command_duration_s"]
        )
        assert descriptor["controllable_component_id"] == (
            component["controllable_component_id"]
        )
        assert descriptor["command_target_id"] == target["command_target_id"]
        for key in ("source_coordinate_name", "coordinate_kind", "min_position", "max_position"):
            assert descriptor[key] == resource[key]
        assert descriptor["source_coordinate_name"] == "robotiq_85_left_knuckle_joint"
        assert descriptor["coordinate_kind"] == "angular_rad"
        assert descriptor["min_position"] == pytest.approx(0.0)
        assert descriptor["max_position"] == pytest.approx(0.8)
        observed = _status_observation(snapshot, presentation="gripper")
        assert observed["kind"] == "coordinate"
        assert observed["complete"] is True
        assert observed["source_coordinate_name"] == descriptor["source_coordinate_name"]
        assert observed["source_epoch"] and not observed["source_epoch"].startswith("render.")
        assert type(observed["sequence"]) is int and observed["sequence"] > 0
        assert observed["position"]["kind"] == "angular_rad"
        assert math.isfinite(observed["position"]["value_rad"])
        return target

    def preset_position(target: dict[str, Any], reference: str) -> float:
        descriptor = target["command"]["target_descriptor"]
        presets = [preset for preset in descriptor["named_presets"]
                   if preset["name"] == descriptor[reference]]
        assert len(presets) == 1
        position = presets[0]["master_position"]
        assert math.isfinite(position)
        assert descriptor["min_position"] <= position <= descriptor["max_position"]
        return position

    def opposite_position(snapshot: dict[str, Any], target: dict[str, Any]) -> float:
        observed = _status_observation(snapshot, presentation="gripper")
        candidates = [preset_position(target, key)
                      for key in ("open_state_ref", "closed_state_ref")]
        assert candidates[0] != candidates[1]
        return max(candidates, key=lambda value: abs(value - observed["position"]["value_rad"]))

    def observed_reached(before: dict[str, Any], goal: float) -> None:
        initial = _status_observation(before, presentation="gripper")
        deadline = time.monotonic() + 5.0
        while True:
            current = client.diagnostics()["status"]
            position_target(current)
            assert current["identity"] == before["identity"]
            observed = _status_observation(current, presentation="gripper")
            assert observed["source_epoch"] == initial["source_epoch"]
            if (observed["sequence"] > initial["sequence"] and
                    observed["position"]["value_rad"] == pytest.approx(goal)):
                return
            assert time.monotonic() < deadline, current
            time.sleep(0.005)

    def completed_and_retired(
        operation_id: str, allowed_states: frozenset[str] | set[str] = frozenset({"completed"}),
    ) -> dict[str, Any]:
        deadline = time.monotonic() + 5.0
        first_terminal = None
        while True:
            projection = client.read(operation_id)
            operation = projection["operation"]
            assert operation["participants"][0]["resources"]
            if operation["state"] in TERMINAL_STATES:
                if first_terminal is None:
                    first_terminal = operation["state"]
                assert operation["state"] == first_terminal, projection
                assert first_terminal in allowed_states, projection
            else:
                # The real command can finish before its first read. Never
                # invent an Executing window or physical feedback evidence.
                assert projection["resident"] is True, projection
            if projection["resident"] is False:
                break
            assert time.monotonic() < deadline, projection
            time.sleep(0.005)
        expected_control = "reset" if operation["state"] == "stopped" else "acknowledge_completed"
        assert projection["last_control"]["control"] == expected_control
        assert projection["last_control"]["operation_id"] == operation_id
        assert operation["operation_revision"] == projection["last_control"][
            "operation_revision_after_acceptance"
        ]
        # The bounded journal coalesces earlier state observations. Its control
        # acceptance captures the already clean terminal state and exact CAS;
        # a separate retained terminal row is not required before Reset/ACK.
        with sqlite3.connect(state_root / "operation-journal.sqlite3") as database:
            records = [json.loads(row[0]) for row in database.execute(
                "SELECT payload_json FROM journal_records "
                "WHERE operation_id=? ORDER BY sequence", (operation_id,),
            )]
        admissions = [(index, record) for index, record in enumerate(records)
                      if record.get("record_kind") == "operation_accepted"]
        controls = [(index, record) for index, record in enumerate(records)
                    if record.get("record_kind") == "control_accepted"]
        assert len(admissions) == len(controls) == 1
        admission_index, admission = admissions[0]
        control_index, control = controls[0]
        assert admission_index < control_index
        accepted_state = admission["operation_state"]
        terminal_state = control["operation_state"]
        assert first_terminal == terminal_state["state"] == operation["state"]
        assert terminal_state["state"] in allowed_states
        assert terminal_state["operation_id"] == operation_id
        assert int(terminal_state["journal_capture_order"]) > int(
            accepted_state["journal_capture_order"]
        )
        owner_epoch = control["owner_epoch"]
        assert owner_epoch == admission["owner_epoch"]
        intent = control["intent"]
        context = intent["request_context"]
        assert context["source_label"] == "kernel.manual_settlement"
        assert context["request_owner_epoch"] == owner_epoch
        assert context["correlation_id"] == intent["operation_id"] == operation_id
        assert intent["request_attribution"]["transport"] == "internal"
        accepted = control["result"]
        assert accepted["request_id"] == context["request_id"]
        assert accepted["replay_status"] == "accepted_new"
        assert accepted["control"] == expected_control
        assert accepted["operation_id"] == operation_id
        assert accepted["accepted_against_revision"] == intent["expected_operation_revision"]
        assert accepted["operation_revision_after_acceptance"] == (
            accepted["accepted_against_revision"] + 1
        )
        assert terminal_state["operation_revision"] == operation["operation_revision"] == (
            accepted["operation_revision_after_acceptance"]
        )
        assert terminal_state["updated_at_ns"] == accepted["accepted_at_ns"]
        assert accepted == projection["last_control"]
        # effect_pending is a durable acceptance flag, not evidence of movement.
        # Position ACK has no provider effect; normal Stopped Reset retains its
        # existing scoped invalidation/authorization and retirement contract.
        assert admission["effect_pending"] is True and control["effect_pending"] is True
        identity_fields = (
            "participant_id", "command_id", "command_generation",
            "controllable_component_id", "command_target_id", "capability_id",
            "control_domain_id", "control_domain_revision", "control_binding_id",
            "control_binding_revision", "capability_provider_binding_id",
            "capability_provider_binding_revision", "payload_kind", "resources",
        )
        assert len(accepted_state["participants"]) == len(operation["participants"]) == 1
        owner = accepted_state["participants"][0]
        assert owner["resources"] == [row["resource_id"]
                                      for row in command_binding["resource_bindings"]]
        for key in ("controllable_component_id", "command_target_id", "control_domain_id",
                    "control_binding_id", "control_binding_revision"):
            assert owner[key] == command_binding[key]
        assert owner["capability_id"] == command_binding["capability_id"]
        for record in records:
            if "operation_state" not in record:
                continue
            state = record["operation_state"]
            assert record["owner_epoch"] == owner_epoch
            assert state["operation_id"] == operation_id
            for key in ("program_binding_id", "program_run_id"):
                assert state[key] == accepted_state[key] == operation[key]
            assert len(state["participants"]) == 1
            assert all(state["participants"][0][key] == owner[key] for key in identity_fields)
            if state["state"] in TERMINAL_STATES:
                assert state["state"] == first_terminal
        assert all(operation["participants"][0][key] == owner[key] for key in identity_fields)
        assert len(terminal_state["participants"]) == 1
        assert terminal_state["participants"][0]["state"] == first_terminal
        assert terminal_state["participants"][0]["terminal_error"] is None
        assert operation["participants"][0]["terminal_error"] is None
        assert CellAdmissionClient(run_root).get()["authoring_allowed"] is True
        return projection

    def component_state() -> dict[str, Any]:
        model = ExecutionCommandClient(run_root).component_model()
        (state,) = [row for row in model["component_states"]
                    if row["component_id"] == component_id]
        return state

    def lifecycle(action: str, _request_id: str) -> dict[str, Any]:
        """One component action; its result is `last_action`."""
        previous = component_state().get("last_action")
        accepted = ExecutionCommandClient(run_root).component_action(component_id, action)
        assert accepted["ok"] is True, accepted
        deadline = time.monotonic() + 10.0
        while True:
            current = component_state()
            last = current.get("last_action")
            if "action" not in current and last is not None and last != previous:
                assert last["action"] == action and last["outcome"] == "succeeded", current
                return current
            assert time.monotonic() < deadline, current
            time.sleep(0.01)

    # Reuse the existing Manager fixture's private preview/public Apply,
    # Program Runner liveness, immutable artifacts and normal ProcessStack owner.
    with TemporaryDirectory(prefix="mf-robotiq-") as temporary:
        owner_root = Path(temporary)
        try:
            with _assistant_stack(
                owner_root, package_root=package,
                metadata_environment="MANYFORGE_ROBOTIQ_POSITION_METADATA",
                package_environment="MANYFORGE_ROBOTIQ_POSITION_PACKAGE_ROOT",
                startup_scenario="program_apply_fixture",
            ) as stack:
                assert stack.fixture_metadata is not None
                assert all(stack.fixture_metadata[key] == value for key, value in metadata.items())
                state_root, run_root = stack.execution_state_root, stack.execution_run_root
                client = ExecutionCommandClient(run_root, timeout_seconds=2.0)
                lifecycle("prepare", "robotiq.initial.prepare")
                lifecycle("enable", "robotiq.initial.enable")
                status = client.diagnostics()["status"]
                target = position_target(status)
                goal = opposite_position(status, target)
                request = OperationIntentRequest.model_validate(
                    _direct_control_gripper_operation(
                        status, request_id="gripper-only.command", position=goal
                    )
                )
                before_preview_events = client.events(status["identity"]["owner_epoch"], 0)["events"]
                preview = client.preview_capability_command(request, trusted_principal_id=None)
                assert preview["ok"] is True, preview
                receipt = preview["preview_receipt"]
                assert receipt["commit_eligible"] is True
                assert receipt["target_position"] == pytest.approx(goal)
                assert receipt["program_binding_id"] == target["command"]["manual_control"][
                    "program_binding_id"
                ]
                assert receipt["request_owner_epoch"] == status["identity"]["owner_epoch"]
                assert receipt["source_coordinate_name"] == target["command"]["resource"][
                    "source_coordinate_name"
                ]
                assert receipt["coordinate_kind"] == target["command"]["resource"]["coordinate_kind"]
                assert client.events(status["identity"]["owner_epoch"], 0)["events"] == before_preview_events
                after_preview = client.diagnostics()["status"]
                assert after_preview["identity"] == status["identity"]
                assert position_target(after_preview)["command"] == target["command"]
                initial_observed = _status_observation(status, presentation="gripper")
                preview_observed = _status_observation(after_preview, presentation="gripper")
                assert preview_observed["source_epoch"] == initial_observed["source_epoch"]
                assert preview_observed["position"] == initial_observed["position"]
                assert after_preview["control_state"]["manual_operations"] == status[
                    "control_state"
                ]["manual_operations"]
                accepted = client.submit(request, trusted_principal_id=None)
                assert accepted["ok"] is True, accepted
                completed_and_retired(accepted["accepted"]["operation_id"])
                observed_reached(status, goal)

                # Addressed Position Stop uses this exact known operation. A natural
                # completion race is not in-flight Stop proof or a cell-wide FullStop.
                current = client.diagnostics()["status"]
                target = position_target(current)
                stop_intent = _direct_control_gripper_operation(
                    current, request_id="gripper-only.stop-command",
                    position=opposite_position(current, target),
                )
                stop_intent["participants"][0]["payload"]["caller_options"][0]["value_s"] = 2.0
                started = client.submit(
                    OperationIntentRequest.model_validate(stop_intent), trusted_principal_id=None,
                )
                assert started["ok"] is True, started
                before_stop = client.read(started["accepted"]["operation_id"])
                assert before_stop["operation"]["state"] not in TERMINAL_STATES or (
                    before_stop["operation"]["state"] == "completed"
                ), before_stop
                stopped = client.stop(
                    StopIntentRequest.model_validate({
                        "request_context": {
                            "request_id": "gripper-only.stop",
                            "request_owner_epoch": current["identity"]["owner_epoch"],
                            "source_label": "native-gripper-only-test",
                        },
                        "operations": [{
                            "operation_id": started["accepted"]["operation_id"],
                            "expected_operation_revision": before_stop["operation"]["operation_revision"],
                            "expected_stop_generation": before_stop["operation"]["active_stop_generation"] or 0,
                        }],
                    }),
                    trusted_principal_id=None,
                )
                assert stopped["ok"] is True, stopped
                resolution = stopped["accepted"]
                reasons = {"exact": "exact_current_scope",
                           "stale_but_unambiguously_resolved": "stale_metadata_current_scope",
                           "already_terminal_no_effect": "known_terminal_or_idle_no_effect"}
                assert resolution["resolution_disposition"] in reasons, stopped
                assert resolution["resolution_reason"] == reasons[resolution["resolution_disposition"]]
                deadline = time.monotonic() + 3.0
                while True:
                    after_stop = client.diagnostics()["status"]
                    stop_action = after_stop["control_state"]["cell_stop_action"]
                    terminal = stop_action["latest_terminal"]
                    if terminal is not None and terminal["certificate"]["stop_id"] == (
                        stopped["accepted"]["stop_id"]
                    ):
                        break
                    assert time.monotonic() < deadline, after_stop
                    time.sleep(0.005)
                assert terminal["certificate"]["stop_generation"] == stopped["accepted"]["stop_generation"]
                # The terminal summary exposes a certificate reference; scope
                # resolution belongs to the checked Stop acceptance above.
                assert set(terminal["certificate"]) == {
                    "stop_certificate_id", "stop_certificate_revision", "stop_id", "stop_generation",
                }
                assert terminal["certificate"]["stop_certificate_id"]
                assert terminal["certificate"]["stop_certificate_revision"] > 0
                assert terminal["result"] == "succeeded_nominal"
                assert terminal["escalated"] is False
                assert terminal["hard_stop_floor_used"] is False
                stop_operation_id = started["accepted"]["operation_id"]
                settled = completed_and_retired(stop_operation_id, {"stopped", "completed"})
                matching_stages = [stage for stage in terminal["executed_stages"]
                                   if stage["operation_id"] == stop_operation_id]
                if settled["operation"]["state"] == "stopped":
                    assert settled["operation"]["active_stop_id"] == stopped["accepted"]["stop_id"]
                    assert matching_stages, terminal
                    assert all(stage["outcome"] == "succeeded" for stage in matching_stages)
                    # Core's existing clean manual settlement owns the normal Reset.
                    assert settled["last_control"]["control"] == "reset"
                else:
                    observed_reached(current, stop_intent["participants"][0]["payload"][
                        "command_value"
                    ]["position"]["value_rad"])
                    assert settled["last_control"]["control"] == "acknowledge_completed"

                # A real Stopped Position command has now completed normal Reset and
                # retirement. A naturally Completed command has its ordinary ACK.
                after_stop = client.diagnostics()["status"]
                target = position_target(after_stop)
                continued_goal = opposite_position(after_stop, target)
                continued = client.submit(
                    OperationIntentRequest.model_validate(_direct_control_gripper_operation(
                        after_stop, request_id="gripper-only.command-after-stop",
                        position=continued_goal,
                    )), trusted_principal_id=None,
                )
                assert continued["ok"] is True, continued
                completed_and_retired(continued["accepted"]["operation_id"])
                observed_reached(after_stop, continued_goal)

                disabled = lifecycle(
                    "disable", "gripper-only.disable",
                )
                assert disabled["lifecycle"] == "prepared", disabled
                after = client.diagnostics()["status"]
                target = position_target(after)
                operations_before_refusal = after["control_state"]["manual_operations"]
                refused = client.submit(
                    OperationIntentRequest.model_validate(
                        _direct_control_gripper_operation(
                            after, request_id="gripper-only.disabled-command",
                            position=preset_position(target, "closed_state_ref"),
                        )
                    ), trusted_principal_id=None,
                )
                assert refused["ok"] is False, refused
                assert refused["error"]["details"]["reason"] == (
                    "component_not_enabled"
                ), refused
                assert client.diagnostics()["status"]["control_state"][
                    "manual_operations"
                ] == operations_before_refusal
                lifecycle(
                    "enable", "gripper-only.enable-again",
                )
                enabled = client.diagnostics()["status"]
                target = position_target(enabled)
                retried_goal = opposite_position(enabled, target)
                retried = client.submit(
                    OperationIntentRequest.model_validate(
                        _direct_control_gripper_operation(
                            enabled, request_id="gripper-only.command-after-enable",
                            position=retried_goal,
                        )
                    ), trusted_principal_id=None,
                )
                assert retried["ok"] is True, retried
                completed_and_retired(retried["accepted"]["operation_id"])
                observed_reached(enabled, retried_goal)
        except BaseException:
            keep_failure_evidence(owner_root)
            raise


def test_logical_zero_component_scenario_applies_and_runs_without_robot_launch(
    tmp_path: Path,
) -> None:
    from manyforge_behavior.catalog import NodeCatalog
    from manyforge_composer.backend.authoring_forest import empty_authoring
    from manyforge_composer.backend.scenario_artifacts import (
        build_scenario_document,
        save_scenario_artifact,
    )

    # Use the normal Scenario writer and activation preparer. The latter owns
    # retention of the real empty model map; this test supplies no sealed DTO.
    authoring = empty_authoring(NodeCatalog(NODE_CATALOG))
    authoring.update({
        "program_id": "logical_process_program",
        "name": "Logical process Program",
        # A Scenario's Program states its assistant disposition.
        "compatibility": {"assistant": "forbidden"},
        "forest": [{"node_id": "timer", "instance_id": "logical.wait",
                    "parameters": {"duration_s": 0.05}, "children": []}],
    })
    scenario_id = "logical_process_scenario"
    scenario = tmp_path / f"{scenario_id}.scenario"
    save_scenario_artifact(
        build_scenario_document(
            scenario_id=scenario_id, name=scenario_id, description="",
            program_draft=authoring,
            deployment_draft={
                "api_version": "manyforge.deployment_intent.v2",
                "deployment_artifact_id": "logical_process_deployment",
                "name": "Logical process Deployment",
                "attachments": [], "components": [], "policies": {},
            },
            manyforge_version="0.1.0", last_known_status="ready",
        ),
        str(scenario), allowed_roots=[str(tmp_path)], overwrite=False,
    )
    # POSIX socket paths are bounded: pytest's tmp_path is too long for the
    # stack's sockets, so the stack root is short.
    short_root = TemporaryDirectory(prefix="mf-logical-")
    stack_root = Path(short_root.name) / "stack"
    stack_root.mkdir()
    metadata = json.loads(_required_path("MANYFORGE_MANAGER_APPLY_METADATA").read_text("utf-8"))
    stack = ProcessStack(
        stack_root, package_root=_required_path("MANYFORGE_MANAGER_APPLY_PACKAGE_ROOT"),
        cell_id=metadata["cell_id"], enable_program_runner=True,
        composer_fs_roots=(tmp_path,), freshness_milliseconds=120_000,
    )
    try:
        stack.prepare_native_manager_fixture()
        stack.start_manager()
        stack.start_program_runner()
        stack.start_api()
        stack.wait_manager_source()
        stack.wait_program_runner()
        stack.wait_authoring_admission(expected_allowed=True)
        stack.start_composer()
        stack.wait_composer()
        with (
            HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as composer,
            HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless,
        ):
            # Load waits for the whole Apply.
            loaded = composer.request("POST", "/api/scenario/load", body={"path": str(scenario)})
            assert loaded["outcome"] == "succeeded", loaded
            assert loaded["components"] == [], loaded
            applied = composer.program_applied()
            assert applied["scenario_id"] == scenario_id, applied
            assert applied["program_id"] == authoring["program_id"], applied
            active = headless.manager_status()["status"]["active"]
            assert applied["retained_bundle_id"] == active["retained_bundle_id"], (applied, active)
            selected = headless.program_applied_status()
            assert selected["executable"] is True and selected["non_executable"] is None, selected
            assert selected["program_id"] == applied["program_id"], selected
            assert selected["program_binding_id"] == applied["program_binding_id"], selected
            assert selected["program"] is None, selected
            assert applied["program"]["api_version"] == "manyforge.program.v3", applied
            assert "component_roles" not in applied["program"], applied
            assert "target_roles" not in applied["program"], applied
            graph = headless.manager_graph()["cell_graph"]
            for field in ("controllable_components", "control_domains", "control_bindings", "model_instances"):
                assert graph[field] == [], graph
            binding = applied["program_binding"]
            assert binding["program_binding_id"] == applied["program_binding_id"], binding
            assert binding["program_revision"] == applied["program_revision"], binding
            assert binding["api_version"] == "manyforge.program_binding.v3", binding
            assert binding["component_bindings"] == [], binding
            assert binding["target_bindings"] == [], binding
            stack.wait_authoring_admission(expected_allowed=True)
            draft = composer.request("GET", "/api/program/draft")["program"]
            started = headless.program_run_start({
                "request_id": "logical.run", "request_owner_epoch": "logical.process.epoch",
                "source_label": "logical_process_test", "correlation_id": "logical.run",
                "scenario_id": scenario_id,
                "source_document_digest": applied["source_document_digest"],
                "expected_applied_record_digest": applied["source"]["record_digest"],
                "draft_program_revision": draft["program_revision"],
            })
            capture = started["capture"]
            assert capture["program_id"] == applied["program_id"], started
            assert capture["program_revision"] == applied["program_revision"], started
            assert capture["program_binding_id"] == applied["program_binding_id"], started
            deadline = time.monotonic() + 10.0
            while True:
                terminal = headless.program_run(capture["program_run_id"])
                if terminal["state"] in {"succeeded", "failed", "stopped"}:
                    assert terminal["state"] == "succeeded", terminal
                    break
                assert time.monotonic() < deadline, terminal
                time.sleep(0.01)
            assert terminal["capture"] == capture, terminal
            assert not list(stack.root.rglob("controller-launch-artifacts")), stack.root
            assert not list(stack.root.rglob("controller_launch_artifacts.json")), stack.root
    except BaseException:
        stack.log.flush()
        keep_failure_evidence(stack_root)
        raise
    finally:
        stack.close()
        short_root.cleanup()
