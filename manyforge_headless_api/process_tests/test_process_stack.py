"""Process-boundary H0 qualification for Manager, API, CLI, and Composer."""

from __future__ import annotations

from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
from queue import Empty, Queue
import shutil
import socket
import subprocess
import sys
from threading import Event, Thread
import time
from typing import Callable, Iterator

import pytest
from websockets.sync.client import connect as connect_websocket

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
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
        self.program_selection_state_root = root / "program-selection-state"
        for path in (
            self.manager_root,
            self.manager_run_root,
            self.execution_state_root,
            self.execution_run_root,
            self.publication_root,
            self.artifact_root,
            self.program_selection_state_root,
        ):
            path.mkdir()
        self.scene_database = root / "logical-scene.sqlite3"
        self.program_cycle_journal = root / "program-cycle.sqlite3"
        self.composer_authoring_root = root / "composer-authoring"
        self.composer_authoring_root.mkdir()
        self.authoring_transaction_journal = (
            self.composer_authoring_root
            / "composer-authoring-transactions.sqlite3"
        )
        self.program_runner_liveness_socket = (
            self.manager_run_root / "program-runner-liveness.sock"
        )
        self.history_database = root / "history.sqlite3"
        self.manager_executable = _required_path("MANYFORGE_MANAGER_DAEMON")
        self.package_root = package_root or _required_path(
            "MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"
        )
        self.manager_package_root = _required_path(
            "MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"
        )
        self.manager_admission_filename: str | None = None
        self.fixture_metadata: dict | None = None
        if not self.package_root.exists():
            raise RuntimeError(f"retained package root does not exist: {self.package_root}")
        self.cell_id = cell_id
        self.program_path = program_path
        self.freshness_milliseconds = freshness_milliseconds
        self.enable_program_runner = enable_program_runner
        self.port = _free_loopback_port()
        self.program_runner_port = _free_loopback_port()
        self.composer_port = _free_loopback_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.program_runner_url = (
            f"http://127.0.0.1:{self.program_runner_port}"
        )
        self.composer_url = f"http://127.0.0.1:{self.composer_port}"
        self.manager: subprocess.Popen[bytes] | None = None
        self.api: subprocess.Popen[bytes] | None = None
        self.program_runner: subprocess.Popen[bytes] | None = None
        self.composer: subprocess.Popen[bytes] | None = None
        self.log = (root / "process-stack.log").open("wb")

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
        self.manager_package_root = self.package_root
        assert (
            self.manager_package_root / self.manager_admission_filename
        ).is_file()

        installed_root = self.root / "installed-targets"
        installed_root.mkdir()
        installed_sources = {
            "manyforge_manager_daemon": self.manager_executable,
            "manyforge_native_execution_host": _required_path(
                "MANYFORGE_NATIVE_EXECUTION_HOST"
            ),
            "manyforge_roboplan_candidate_worker": _required_path(
                "MANYFORGE_ROBOPLAN_CANDIDATE_WORKER"
            ),
        }
        for installed_name, source in installed_sources.items():
            assert source.is_file() and os.access(source, os.X_OK)
            shutil.copy2(source, installed_root / installed_name)
        self.manager_executable = installed_root / "manyforge_manager_daemon"

    def start_manager(
        self, *, publication_root: Path | None = None
    ) -> None:
        assert self.manager is None
        manager_publication_root = publication_root or self.publication_root
        manager_publication_root.mkdir(exist_ok=True)
        command = [
            str(self.manager_executable),
            "--install-root",
            str(self.manager_package_root),
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
            "--authoring-transaction-journal",
            str(self.authoring_transaction_journal),
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
        self.manager = subprocess.Popen(
            command,
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )

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
            "--program-selection-state-root",
            str(self.program_selection_state_root),
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
        if self.enable_program_runner:
            command.extend(
                (
                    "--program-runner-url",
                    self.program_runner_url,
                    "--program-runner-timeout-s",
                    "8",
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
            if (
                self.program_runner is not None
                and self.program_runner.poll() is not None
            ):
                raise RuntimeError(
                    "Program runner exited with "
                    f"{self.program_runner.returncode}"
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
                    isinstance(unavailable, list)
                    and unavailable_section in unavailable
                )
                if runtime.get("complete") is expected and section_observed:
                    return runtime
            except HeadlessClientError as exc:
                last_error = exc
            time.sleep(0.01)
        raise RuntimeError(
            f"runtime did not reach complete={expected}: {last_error}"
        )

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
                last_error = RuntimeError(
                    f"last authoring admission was {admission!r}"
                )
            except HeadlessClientError as error:
                last_error = error
            time.sleep(0.01)
        raise RuntimeError(
            f"authoring admission did not become available: {last_error}"
        )

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
            "--authoring-state-root",
            str(self.composer_authoring_root),
        ]
        if self.program_path is not None:
            command.extend(("--program-path", str(self.program_path)))
        self.composer = subprocess.Popen(
            command,
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=os.environ.copy(),
        )

    def wait_composer(self, timeout_seconds: float = 8.0) -> dict:
        deadline = time.monotonic() + timeout_seconds
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            if self.composer is not None and self.composer.poll() is not None:
                raise RuntimeError(
                    f"Composer exited with {self.composer.returncode}"
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
        request = (
            b'{"api_version":"manyforge.manager_command.v1",'
            b'"command":"shutdown_runtime"}'
        )
        try:
            with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as client:
                client.settimeout(1.0)
                client.connect(
                    str(self.manager_run_root / "manager-command.sock")
                )
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
    root: Path, *, package_root: Path | None = None
) -> Iterator[ProcessStack]:
    selected_package_root = package_root or _required_path(
        "MANYFORGE_ASSISTANT_NATIVE_PACKAGE_ROOT"
    )
    retained_programs = sorted(
        (selected_package_root / "programs" / "sha256").glob("*.json")
    )
    assert len(retained_programs) == 1
    stack = ProcessStack(
        root,
        package_root=selected_package_root,
        cell_id="i6_lite6_arm_gripper_aggregate_cell",
        program_path=retained_programs[0],
        freshness_milliseconds=120000,
        enable_program_runner=True,
    )
    try:
        stack.prepare_native_manager_fixture(
            metadata_environment="MANYFORGE_ASSISTANT_NATIVE_METADATA",
            package_environment="MANYFORGE_ASSISTANT_NATIVE_PACKAGE_ROOT",
        )
        assert stack.manager_package_root == stack.package_root
        stack.start_manager()
        stack.start_program_runner()
        stack.start_api()
        initial = stack.wait_manager_source()
        stack.wait_program_runner()
        initial_admission = stack.wait_authoring_admission(expected_allowed=True)
        assert stack.fixture_metadata is not None
        metadata = stack.fixture_metadata
        assert initial["status"]["active"]["retained_bundle_id"] == (
            metadata["predecessor_retained_bundle_id"]
        )
        with HeadlessHttpClient(stack.base_url, timeout_seconds=20.0) as client:
            apply_body = {
                    "request_id": "assistant-native.process.apply",
                    "request_owner_epoch": initial["source"][
                        "source_owner_epoch"
                    ],
                    "source_label": "assistant_native_process_test",
                    "correlation_id": "assistant-native.process.apply",
                    "deployment_artifact_id": metadata[
                        "physical_deployment_artifact_id"
                    ],
                    "deployment_revision": metadata[
                        "physical_deployment_revision"
                    ],
                    "expected_current": {
                        "graph_revision": metadata[
                            "predecessor_graph_revision"
                        ],
                        "retained_bundle_id": metadata[
                            "predecessor_retained_bundle_id"
                        ],
                    },
                    "reject_if_affected_busy": True,
                    "expected_admission_generation": initial_admission[
                        "admission_generation"
                    ],
                }
            preview = client.manager_deployment_candidate_preview(
                {
                    "deployment_artifact_id": apply_body[
                        "deployment_artifact_id"
                    ],
                    "deployment_revision": apply_body["deployment_revision"],
                    "expected_current": apply_body["expected_current"],
                    "expected_admission_generation": apply_body[
                        "expected_admission_generation"
                    ],
                    "request_owner_epoch": apply_body["request_owner_epoch"],
                }
            )
            apply_result = client.manager_apply(
                {**apply_body, "candidate_receipt": preview["candidate_receipt"]}
            )
            assert apply_result["ok"] is True
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            active = stack.wait_manager_source()["status"]["active"]
            if active["retained_bundle_id"] == metadata[
                "physical_candidate_retained_bundle_id"
            ]:
                break
            time.sleep(0.02)
        else:
            raise RuntimeError("assistant-native physical Apply did not converge")
        admission = stack.wait_authoring_admission(expected_allowed=True)
        assert admission["authoring_allowed"] is True
        assert admission["admission_generation"] > 0
        with HeadlessHttpClient(stack.base_url) as client:
            with pytest.raises(HeadlessClientError) as stale:
                client.acquire_authoring_transaction(
                    {
                        "request_id": "assistant-native.stale-generation",
                        "request_owner_epoch": "assistant-native.stale.epoch",
                        "source_label": "assistant_native_process_test",
                        "correlation_id": "assistant-native.stale-generation",
                        "expected_admission_generation": initial_admission[
                            "admission_generation"
                        ],
                        "request_payload_digest": "sha256:" + "0" * 64,
                        "owner_transaction_id": "assistant-native.stale",
                    }
                )
            assert stale.value.code == "stale_generation"
        assert (stack.execution_run_root / "execution-command.sock").is_socket()
        assert (stack.execution_run_root / "execution-observation.sock").is_socket()
        assert stack.authoring_transaction_journal.parent == (
            stack.composer_authoring_root
        )
        stack.start_composer()
        stack.wait_composer()
        yield stack
    finally:
        stack.close()


@contextmanager
def _composer_snapshot_stream(
    stack: ProcessStack, *, client_id: str
) -> Iterator[Callable[[], dict]]:
    websocket_url = (
        stack.composer_url.replace("http://", "ws://", 1)
        + "/ws/control"
    )
    with connect_websocket(
        websocket_url,
        open_timeout=2,
        close_timeout=1,
        max_size=16 * 1024 * 1024,
    ) as websocket:
        websocket.send(
            json.dumps(
                {
                    "schema_version": "manyforge.execution_control.v2",
                    "message_kind": "client_hello",
                    "supported_versions": ["manyforge.execution_control.v2"],
                    "client_incarnation": f"process-test.{client_id}",
                    "source_label": "process_test",
                }
            )
        )
        server_hello = json.loads(websocket.recv(timeout=2.0))
        assert server_hello["message_kind"] == "server_hello"
        control_session_id = server_hello["control_session_id"]
        client_sequence = 0
        stopped = Event()
        current_shell: Queue[dict | BaseException] = Queue(maxsize=1)
        applied_shell: dict | None = None

        def publish(value: dict | BaseException) -> None:
            try:
                current_shell.get_nowait()
            except Empty:
                pass
            current_shell.put_nowait(value)

        def receive() -> None:
            nonlocal applied_shell, client_sequence
            try:
                while not stopped.is_set():
                    try:
                        last_payload = json.loads(websocket.recv(timeout=0.5))
                    except TimeoutError:
                        continue
                    assert isinstance(last_payload, dict)
                    if last_payload.get("message_kind") == "heartbeat_challenge":
                        client_sequence += 1
                        websocket.send(
                            json.dumps(
                                {
                                    "schema_version": (
                                        "manyforge.execution_control.v2"
                                    ),
                                    "message_kind": "heartbeat_echo",
                                    "control_session_id": control_session_id,
                                    "client_sequence": str(client_sequence),
                                    "challenge_sequence": last_payload[
                                        "challenge_sequence"
                                    ],
                                }
                            )
                        )
                        continue
                    kind = last_payload.get("message_kind")
                    shell = last_payload.get("shell")
                    if kind not in {"snapshot", "delta"} or not isinstance(
                        shell, dict
                    ):
                        continue
                    if kind == "snapshot":
                        assert shell.get("complete") is True
                        applied_shell = dict(shell)
                    else:
                        assert applied_shell is not None
                        assert shell.get("complete") is False
                        applied_shell.update(
                            {
                                name: value
                                for name, value in shell.items()
                                if name != "complete"
                            }
                        )
                    state_sequence = last_payload.get("state_sequence")
                    if state_sequence is not None:
                        websocket.send(
                            json.dumps(
                                {
                                    "schema_version": (
                                        "manyforge.execution_control.v2"
                                    ),
                                    "message_kind": "state_applied",
                                    "control_session_id": control_session_id,
                                    "state_sequence": state_sequence,
                                }
                            )
                        )
                    publish(dict(applied_shell))
            except BaseException as error:
                if not stopped.is_set():
                    publish(error)

        receiver = Thread(
            target=receive,
            name="manyforge-process-test-control-receiver",
            daemon=True,
        )
        receiver.start()

        def next_snapshot() -> dict:
            try:
                value = current_shell.get(timeout=8.0)
            except Empty as error:
                raise RuntimeError(
                    "Composer did not publish a complete current snapshot"
                ) from error
            if isinstance(value, BaseException):
                raise RuntimeError("Composer control stream failed") from value
            return value

        try:
            yield next_snapshot
        finally:
            stopped.set()
            receiver.join(timeout=2.0)
            if receiver.is_alive():
                raise RuntimeError("Composer control receiver did not stop")


def _composer_fence(
    snapshot: dict,
    *,
    request_id: str,
) -> dict:
    draft_identity = snapshot["draft_identity"]
    admission = snapshot["authoring_admission"]
    return {
        "draftId": draft_identity["draft_id"],
        "expectedDraftRevision": draft_identity["draft_revision"],
        "expectedAdmissionGeneration": admission["admission_generation"],
        "requestOwnerEpoch": "composer.process.epoch.1",
        "requestId": request_id,
    }


def _matching_composer_snapshot(
    next_snapshot: Callable[[], dict], draft: dict
) -> dict:
    for _ in range(32):
        snapshot = next_snapshot()
        if snapshot.get("draft_identity") == {
            "draft_id": draft["draft_id"],
            "draft_revision": draft["draft_revision"],
        }:
            return snapshot
    raise RuntimeError("Composer did not publish the exact current Draft snapshot")


def _matching_assistant_context(
    stack: ProcessStack, composer: HeadlessHttpClient
) -> tuple[dict, dict]:
    selection_fields = (
        "selection_projection_generation",
        "selection_source_content_digest",
        "selection_source_generation",
        "selection_source_owner_epoch",
        "selection_source_owner_id",
    )
    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        with HeadlessHttpClient(stack.base_url) as headless:
            source_context = headless.source_model_context()
        try:
            assistant_mode = composer.assistant_mode("composer-assistant")
        except HeadlessClientError as error:
            if error.code not in {
                "assistant_mode_projection_invalid",
                "assistant_mode_projection_unavailable",
                "node_environment_projection_invalid",
            }:
                raise
            time.sleep(0.01)
            continue
        node_source = source_context["node_environment"]["source"]
        model_source = source_context["source_model"]["source"]
        assistant_source = assistant_mode["source"]
        if all(
            model_source[field] == node_source[field]
            and assistant_source[field] == node_source[field]
            for field in selection_fields
        ):
            return source_context, assistant_mode
        time.sleep(0.01)
    raise RuntimeError("Headless did not publish a coherent assistant context")


def _apply_body(
    client: HeadlessHttpClient,
    owner_epoch: str,
    request_id: str,
    admission_generation: int,
    *,
    preview_owner_epoch: str | None = None,
) -> dict:
    active = client.manager_status()["status"]["active"]
    body = {
        "request_id": request_id,
        "request_owner_epoch": owner_epoch,
        "source_label": "process_test",
        "correlation_id": "correlation.1",
        "deployment_artifact_id": "candidate",
        "deployment_revision": "sha256:" + "a" * 64,
        "expected_current": {
            "graph_revision": active["graph_revision"],
            "retained_bundle_id": active["retained_bundle_id"],
        },
        "reject_if_affected_busy": True,
        "expected_admission_generation": admission_generation,
    }
    preview = client.manager_deployment_candidate_preview(
        {
            "deployment_artifact_id": body["deployment_artifact_id"],
            "deployment_revision": body["deployment_revision"],
            "expected_current": body["expected_current"],
            "expected_admission_generation": admission_generation,
            "request_owner_epoch": preview_owner_epoch or owner_epoch,
        }
    )
    return {**body, "candidate_receipt": preview["candidate_receipt"]}


def _assistant_callback_body(
    *,
    draft: dict,
    node_environment: dict,
    source_model: dict,
    assistant_mode: dict,
    scene_resource_id: str,
    request_id: str,
    admission_generation: int,
) -> dict:
    tool = next(
        row
        for row in assistant_mode["tools"]
        if row["tool_id"] == "scene_draft_add_object"
    )
    return {
        "assistantMode": "composer-assistant",
        "retainedBundleId": node_environment["retained_bundle_id"],
        "graphRevision": node_environment["graph_revision"],
        "nodeEnvironmentDigest": node_environment["environment_digest"],
        "nodeEnvironmentProjectionDigest": node_environment["source"][
            "source_content_digest"
        ],
        "sourceModelProjectionDigest": source_model["source"][
            "source_content_digest"
        ],
        "assistantProjectionDigest": assistant_mode[
            "assistant_projection_digest"
        ],
        "assistantModeProjectionDigest": assistant_mode["source"][
            "source_content_digest"
        ],
        "catalogHash": assistant_mode["catalog_hash"],
        "requestedTools": [],
        "fieldPolicy": {},
        "inputSchemaHash": tool["input_schema_hash"],
        "draftId": draft["draft_id"],
        "expectedDraftRevision": draft["draft_revision"],
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
            source_context, assistant_mode = _matching_assistant_context(
                stack, composer
            )
            node_environment = source_context["node_environment"]
            source_model = source_context["source_model"]
            draft = composer.request("GET", "/api/program/draft")
            manager_before = composer.manager_status()["status"]
            runtime_before = composer.runtime()
            package_before = _plain_package_files(stack.package_root)
            assert stack.fixture_metadata is not None
            active_bundle = stack.fixture_metadata[
                "physical_candidate_retained_bundle_id"
            ]
            active_graph = stack.fixture_metadata["physical_graph_revision"]
            scenarios = composer.scenarios()

            assert {
                row["model_instance_id"] for row in source_model["occurrences"]
            } == {"lite6_fixture_instance"}
            assert {
                row["component_id"] for row in source_model["targets"]
            } == {"lite6_arm"}
            assert node_environment["retained_bundle_id"] == active_bundle
            assert node_environment["graph_revision"] == active_graph
            assert source_model["retained_bundle_id"] == active_bundle
            assert source_model["graph_revision"] == active_graph
            assert assistant_mode["retained_bundle_id"] == active_bundle
            assert assistant_mode["graph_revision"] == active_graph
            assert manager_before["active"]["retained_bundle_id"] == active_bundle
            assert manager_before["active"]["graph_revision"] == active_graph
            assert scenarios["scenarios"][0]["retained_bundle_id"] == active_bundle
            assert scenarios["scenarios"][0]["program_binding_id"] == (
                stack.fixture_metadata["program_binding_id"]
            )
            assert scenarios["scenario_catalog_revision"] == (
                stack.fixture_metadata["scenario_catalog_revision"]
            )
            assert {row["tool_id"] for row in assistant_mode["tools"]} == {
                "program_read",
                "scene_draft_add_object",
            }

            # A refreshed owner projection or admission generation changes the
            # exact payload, so every new attempt needs a new idempotency key.
            for attempt in range(8):
                source_context, assistant_mode = _matching_assistant_context(
                    stack, composer
                )
                node_environment = source_context["node_environment"]
                source_model = source_context["source_model"]
                accepted_body = _assistant_callback_body(
                    draft=draft,
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
            assert accepted["nextAdmissionGeneration"] > accepted_body[
                "expectedAdmissionGeneration"
            ]

            after_accepted = composer.request("GET", "/api/program/draft")
            assert after_accepted["draft_revision"] == draft["draft_revision"] + 1
            assert after_accepted["saved"] is False
            assert after_accepted["applied"] is False
            assert "assistant_workpiece" in {
                row["scene_resource_id"]
                for row in after_accepted["program"]["scene"]
            }
            for attempt in range(8):
                stale_context, stale_mode = _matching_assistant_context(
                    stack, composer
                )
                stale_identity = _assistant_callback_body(
                    draft=after_accepted,
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
                invalid_context, invalid_mode = _matching_assistant_context(
                    stack, composer
                )
                invalid_edit = _assistant_callback_body(
                    draft=after_accepted,
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
            assert rejected["nextAdmissionGeneration"] == invalid_edit[
                "expectedAdmissionGeneration"
            ]
            assert rejected["result"]["kind"] == "validation_error"
            assert composer.request("GET", "/api/program/draft") == after_accepted

            callback_latencies_s: list[float] = []
            for index in range(12):
                for attempt in range(8):
                    read_context, read_mode = _matching_assistant_context(
                        stack, composer
                    )
                    program_read = next(
                        row
                        for row in read_mode["tools"]
                        if row["tool_id"] == "program_read"
                    )
                    read_body = _assistant_callback_body(
                        draft=after_accepted,
                        node_environment=read_context["node_environment"],
                        source_model=read_context["source_model"],
                        assistant_mode=read_mode,
                        scene_resource_id="unused_for_program_read",
                        request_id=f"assistant.process.read.{index}.{attempt}",
                        admission_generation=_current_authoring_generation(headless),
                    )
                    read_body["inputSchemaHash"] = program_read[
                        "input_schema_hash"
                    ]
                    read_body["arguments"] = {}
                    started = time.perf_counter()
                    try:
                        read_result = composer.request(
                            "POST",
                            "/api/assistant/bridge/tools/program_read",
                            body=read_body,
                        )
                        callback_latencies_s.append(
                            time.perf_counter() - started
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
                assert read_result["success"] is True
                assert read_result["draftMutated"] is False
                assert read_result["nextAdmissionGeneration"] == read_body[
                    "expectedAdmissionGeneration"
                ]
            assert max(callback_latencies_s) < 0.5, callback_latencies_s

        stack.kill_composer()
        assert stack.authoring_transaction_journal.is_file()
        stack.start_composer()
        stack.wait_composer()
        with (
            HeadlessHttpClient(stack.composer_url) as replacement,
            HeadlessHttpClient(stack.base_url) as headless,
        ):
            assert replacement.request(
                "GET", "/api/program/draft"
            ) == after_accepted
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

            replacement_context, replacement_mode = _matching_assistant_context(
                stack, replacement
            )
            concurrent_generation = _current_authoring_generation(headless)
            concurrent_bodies = [
                _assistant_callback_body(
                    draft=after_accepted,
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
        accepted_count = sum(
            kind == "accepted" for kind, _detail in concurrent_results
        )
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
        assert len([
            detail
            for kind, detail in concurrent_results
            if kind == "rejected"
        ]) == 2 - accepted_count

        with HeadlessHttpClient(stack.composer_url) as composer:
            final_draft = composer.request("GET", "/api/program/draft")
            concurrent_ids = {
                row["scene_resource_id"]
                for row in final_draft["program"]["scene"]
                if row["scene_resource_id"].startswith("concurrent_object_")
            }
            assert len(concurrent_ids) == accepted_count
            assert final_draft["draft_revision"] == (
                after_accepted["draft_revision"] + accepted_count
            )
            assert final_draft["saved"] is False
            assert final_draft["applied"] is False
            assert composer.manager_status()["status"]["active"] == manager_before[
                "active"
            ]
            runtime_after = composer.runtime()
            assert runtime_after["active_retained_bundle_id"] == runtime_before[
                "active_retained_bundle_id"
            ]
            assert runtime_after["active_graph_revision"] == runtime_before[
                "active_graph_revision"
            ]
            assert _plain_package_files(stack.package_root) == package_before


def test_real_program_apply_reuses_active_deployment_and_survives_api_restart(
    tmp_path: Path,
) -> None:
    package_root = tmp_path / "read-only-package"
    shutil.copytree(
        _required_path("MANYFORGE_ASSISTANT_NATIVE_PACKAGE_ROOT"), package_root
    )
    package_before = _plain_package_files(package_root)
    with _read_only_tree(package_root):
        with _assistant_stack(tmp_path, package_root=package_root) as stack:
            assert stack.wait_complete(True)["complete"] is True
            with _composer_snapshot_stream(
                stack, client_id="process-test-browser"
            ) as next_snapshot, HeadlessHttpClient(
                stack.composer_url,
                timeout_seconds=8.0,
            ) as composer, HeadlessHttpClient(stack.base_url) as client:
                scenarios = client.scenarios()
                assert [row["scenario_id"] for row in scenarios["scenarios"]] == [
                    "program_apply_fixture"
                ]
                with pytest.raises(HeadlessClientError) as absent:
                    client.program_applied()
                assert absent.value.code == "program_applied_selection_not_found"

                unloaded_draft = composer.request("GET", "/api/program/draft")
                for attempt in range(8):
                    assert composer.request(
                        "GET", "/api/program/draft"
                    ) == unloaded_draft
                    load_snapshot = _matching_composer_snapshot(
                        next_snapshot, unloaded_draft
                    )
                    try:
                        loaded = composer.request(
                            "POST",
                            "/api/scenarios/load",
                            body={
                                **_composer_fence(
                                    load_snapshot,
                                    request_id=(
                                        f"program-apply.process.load.{attempt}"
                                    ),
                                ),
                                "scenarioId": "program_apply_fixture",
                                "scenarioCatalogRevision": scenarios[
                                    "scenario_catalog_revision"
                                ],
                                "confirmDiscard": False,
                            },
                        )
                        break
                    except HeadlessClientError as error:
                        if (
                            error.code
                            not in {
                                "authoring_not_ready",
                                "stale_generation",
                            }
                            or attempt == 7
                        ):
                            raise
                assert loaded["status"] == "loaded"
                assert client.program_applied()["scenario_id"] == (
                    "program_apply_fixture"
                )

                initial_draft = composer.request("GET", "/api/program/draft")
                for attempt in range(8):
                    assert composer.request(
                        "GET", "/api/program/draft"
                    ) == initial_draft
                    initial_snapshot = _matching_composer_snapshot(
                        next_snapshot, initial_draft
                    )
                    try:
                        composer.request(
                            "POST",
                            "/api/program/draft/mutations",
                            body={
                                **_composer_fence(
                                    initial_snapshot,
                                    request_id=(
                                        f"program-apply.process.edit.{attempt}"
                                    ),
                                ),
                                "mutation": "tree_draft_update_node_params",
                                "arguments": {
                                    "instance_id": "assistant_native_timer",
                                    "expected_node_id": "timer",
                                    "parameters": {"duration_s": 0.002},
                                },
                            },
                        )
                        break
                    except HeadlessClientError as error:
                        if (
                            error.code
                            not in {
                                "authoring_client_snapshot_stale",
                                "authoring_not_ready",
                                "stale_generation",
                            }
                            or attempt == 7
                        ):
                            raise
                edited_draft = composer.request("GET", "/api/program/draft")
                program = edited_draft["program"]
                package_program_path = (
                    stack.package_root
                    / "programs"
                    / "sha256"
                    / f"{program['program_revision'][7:]}.json"
                )
                publication_program_path = (
                    stack.artifact_root
                    / "programs"
                    / "sha256"
                    / f"{program['program_revision'][7:]}.json"
                )
                publication_binding_path = (
                    stack.artifact_root
                    / "program-bindings"
                    / "sha256"
                )
                for attempt in range(8):
                    assert composer.request(
                        "GET", "/api/program/draft"
                    ) == edited_draft
                    apply_snapshot = _matching_composer_snapshot(
                        next_snapshot, edited_draft
                    )
                    try:
                        applied_response = composer.request(
                            "POST",
                            "/api/program/apply",
                            body={
                                **_composer_fence(
                                    apply_snapshot,
                                    request_id=f"program-apply.process.1.{attempt}",
                                )
                            },
                        )
                        break
                    except HeadlessClientError as error:
                        if (
                            error.code
                            not in {
                                "authoring_client_snapshot_stale",
                                "authoring_not_ready",
                                "stale_generation",
                            }
                            or attempt == 7
                        ):
                            raise
                result = applied_response["headless"]
                assert result["status"] == "applied"
                assert set(result) == {
                    "applied",
                    "preparation",
                    "schema_version",
                    "status",
                }
                assert result["applied"]["program_revision"] == program[
                    "program_revision"
                ]
                first_applied = client.program_applied()
                assert first_applied == result["applied"]
                assert publication_program_path.is_file()
                assert not package_program_path.exists()
                candidate_binding_path = publication_binding_path / (
                    f"{result['applied']['program_binding_id'][7:]}.json"
                )
                package_binding_path = (
                    stack.package_root
                    / "program-bindings"
                    / "sha256"
                    / f"{result['applied']['program_binding_id'][7:]}.json"
                )
                assert candidate_binding_path.is_file()
                assert not package_binding_path.exists()

                stack.kill_api()
                stack.start_api()
                stack.wait_complete(True)
                reopened = client.program_applied()
                assert reopened == first_applied
                replay_draft = composer.request("GET", "/api/program/draft")
                with _composer_snapshot_stream(
                    stack, client_id="process-test-browser-restarted"
                ) as restarted_snapshots:
                    for attempt in range(8):
                        assert composer.request(
                            "GET", "/api/program/draft"
                        ) == replay_draft
                        replay_snapshot = _matching_composer_snapshot(
                            restarted_snapshots, replay_draft
                        )
                        try:
                            replay_response = composer.request(
                                "POST",
                                "/api/program/apply",
                                body={
                                    **_composer_fence(
                                        replay_snapshot,
                                        request_id=(
                                            f"program-apply.process.2.{attempt}"
                                        ),
                                    )
                                },
                            )
                            break
                        except HeadlessClientError as error:
                            if (
                                error.code
                                not in {
                                    "authoring_client_snapshot_stale",
                                    "authoring_not_ready",
                                    "stale_generation",
                                }
                                or attempt == 7
                            ):
                                raise
                replay = replay_response["headless"]
                replay_applied = replay["applied"]
                assert {
                    key: value
                    for key, value in replay_applied.items()
                    if key != "source"
                } == {
                    key: value
                    for key, value in first_applied.items()
                    if key != "source"
                }
                assert replay_applied["source"] == {
                    **first_applied["source"],
                    "source_owner_epoch": replay_applied["source"][
                        "source_owner_epoch"
                    ],
                }
                assert replay_applied["source"]["source_owner_epoch"] != (
                    first_applied["source"]["source_owner_epoch"]
                )

                for attempt in range(8):
                    try:
                        started = client.program_run_start(
                            {
                                "request_id": (
                                    f"program-apply.process.run.1.{attempt}"
                                ),
                                "request_owner_epoch": (
                                    "process-test-browser-restarted"
                                ),
                                "source_label": "program_apply_process_test",
                                "correlation_id": "program-apply.process.run.1",
                                "scenario_id": "program_apply_fixture",
                                "scenario_catalog_revision": scenarios[
                                    "scenario_catalog_revision"
                                ],
                                "expected_applied_record_digest": replay_applied[
                                    "source"
                                ]["record_digest"],
                                "draft_program_revision": replay_applied[
                                    "program_revision"
                                ],
                                "expected_admission_generation": (
                                    stack.wait_authoring_admission(
                                        expected_allowed=True
                                    )["admission_generation"]
                                ),
                            }
                        )
                        break
                    except HeadlessClientError as error:
                        if (
                            error.code
                            not in {"authoring_not_ready", "stale_generation"}
                            or attempt == 7
                        ):
                            raise
                program_run_id = started["capture"]["program_run_id"]
                terminal = started
                deadline = time.monotonic() + 10.0
                while terminal["state"] in {"running", "stopping", "finalizing"}:
                    if time.monotonic() >= deadline:
                        raise RuntimeError(
                            f"Program run did not settle: {terminal!r}"
                        )
                    time.sleep(0.01)
                    terminal = client.program_run(program_run_id)
                assert terminal["state"] == "succeeded"

                cycle = client.program_cycle_status()
                assert cycle["state"] == "terminal_awaiting_reset"
                terminal_reference = cycle["terminal_or_settlement_reference"]
                reset = client.program_cycle_reset(
                    {
                        "request_id": "program-apply.process.reset.1",
                        "request_owner_epoch": "process-test-browser-restarted",
                        "source_label": "program_apply_process_test",
                        "correlation_id": "program-apply.process.run.1",
                        "program_run_id": program_run_id,
                        "expected_owner_epoch": cycle[
                            "program_runner_owner_epoch"
                        ],
                        "expected_cycle_generation": cycle[
                            "cycle_generation"
                        ],
                        "expected_claim_generation": cycle[
                            "cycle_claim_generation"
                        ],
                        "terminal_reference": terminal_reference,
                        "settlement_evidence_digest": terminal_reference,
                    }
                )
                assert reset["program_run_id"] == program_run_id
                assert client.program_cycle_status()["state"] == (
                    "idle_after_reset"
                )
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
            assert environment["graph_revision"] == (
                status["status"]["active"]["graph_revision"]
            )
            assert environment["manifest"]["environment_digest"] == (
                environment["environment_digest"]
            )
            assert environment["catalog"]["catalog_digest"] == (
                environment["catalog_digest"]
            )
            assert environment["availability"]["execution_available"] is False
            assert client.manager_graph()["cell_graph"]["cell_id"] == (
                "c1_build_authority_cell"
            )
            assert client.manager_diff()["converged"] is True
            assert client.scene()["scene_runtime"]["payload"]["state"]["state"] == (
                "open"
            )

            with pytest.raises(HeadlessClientError) as unavailable:
                client.manager_apply(
                    _apply_body(
                        client,
                        first_epoch,
                        "request.1",
                        1,
                    )
                )
            assert unavailable.value.code == "deployment_candidate_rejected"

        stack.start_composer()
        assert stack.wait_composer()["projection_owner_epoch"] == owner_identity[0]
        with HeadlessHttpClient(stack.composer_url) as composer:
            assert composer.manager_graph()["cell_graph"]["cell_id"] == (
                "c1_build_authority_cell"
            )
            assert _owner_identity(
                composer.runtime(),
                composer.alarms(limit=128),
            ) == owner_identity
            assert composer.node_environment()["environment_digest"] == (
                environment["environment_digest"]
            )
            assert composer.request("GET", "/api/catalog")[
                "environmentDigest"
            ] == environment["environment_digest"]
            assert composer.request("GET", "/api/nodes/environment")[
                "active"
            ]["retainedBundleId"] == environment["retained_bundle_id"]

        stack.kill_composer()
        assert stack.authoring_transaction_journal.is_file()
        with HeadlessHttpClient(stack.base_url) as client:
            assert _owner_identity(
                client.runtime(),
                client.alarms(limit=128),
            ) == owner_identity

        stack.start_composer()
        stack.wait_composer()
        with HeadlessHttpClient(stack.composer_url) as replacement_composer:
            assert _owner_identity(
                replacement_composer.runtime(),
                replacement_composer.alarms(limit=128),
            ) == owner_identity

        cli_reads = {
            "health": "manyforge.headless_health.v1",
            "manager-graph": "manyforge.manager_graph_projection.v1",
            "node-environment": "manyforge.node_environment_projection.v2",
            "source-model": "manyforge.source_model_projection.v2",
            "scene": "manyforge.scene_projection.v1",
            "alarms": "manyforge.operator_alarm_snapshot.v2",
            "history": "manyforge.historian_summary_page.v1",
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
        apply_command = subprocess.run(
            [
                sys.executable,
                "-m",
                "manyforge_headless_api",
                "apply",
                "candidate",
                "sha256:" + "a" * 64,
                "current-graph",
                "current-bundle",
                "--request-id",
                "request.cli.1",
                "--request-owner-epoch",
                first_epoch,
                "--url",
                stack.base_url,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
            env=os.environ.copy(),
        )
        assert apply_command.returncode == 69
        assert "execution_owner_unavailable" in apply_command.stderr

        stack.kill_manager()
        stale_runtime = stack.wait_complete(False, unavailable_section="manager")
        assert "manager" in stale_runtime["unavailable_sections"]
        with HeadlessHttpClient(stack.composer_url) as composer:
            assert composer.runtime()["complete"] is False
            with pytest.raises(HeadlessClientError) as unavailable_environment:
                composer.request("GET", "/api/catalog")
            assert unavailable_environment.value.code == (
                "node_environment_projection_unavailable"
            )

        stack.start_manager()
        stack.wait_complete(True)
        with HeadlessHttpClient(stack.base_url) as client:
            replacement = client.manager_status()
            replacement_epoch = replacement["source"]["source_owner_epoch"]
            assert replacement_epoch != first_epoch

            with pytest.raises(HeadlessClientError) as stale:
                _apply_body(
                    client,
                    first_epoch,
                    "request.stale",
                    1,
                )
            assert stale.value.code == "stale_request_owner_epoch"

            with pytest.raises(HeadlessClientError) as unavailable:
                client.manager_apply(
                    _apply_body(
                        client,
                        replacement_epoch,
                        "request.replacement",
                        1,
                    )
                )
            assert unavailable.value.code == "deployment_candidate_rejected"
