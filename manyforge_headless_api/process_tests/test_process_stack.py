"""Process-boundary H0 qualification for Manager, API, CLI, and Composer."""

from __future__ import annotations

from contextlib import contextmanager
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import time
from typing import Iterator

import pytest

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


CATALOG = Path("/workspace/manyforge_headless/resources/operator-alarm-catalog.v1.yaml")
NODE_CATALOG = Path("/workspace/manyforge_behavior/resources/node_catalog.yaml")


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
    def __init__(self, root: Path) -> None:
        self.root = root
        self.manager_root = root / "manager"
        self.runtime_root = root / "runtime"
        self.publication_root = root / "publications"
        for path in (self.manager_root, self.runtime_root, self.publication_root):
            path.mkdir()
        self.scene_database = root / "logical-scene.sqlite3"
        self.history_database = root / "history.sqlite3"
        self.manager_executable = _required_path("MANYFORGE_MANAGER_DAEMON")
        self.package_root = _required_path(
            "MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"
        )
        self.port = _free_loopback_port()
        self.composer_port = _free_loopback_port()
        self.base_url = f"http://127.0.0.1:{self.port}"
        self.composer_url = f"http://127.0.0.1:{self.composer_port}"
        self.manager: subprocess.Popen[bytes] | None = None
        self.api: subprocess.Popen[bytes] | None = None
        self.composer: subprocess.Popen[bytes] | None = None
        self.log = (root / "process-stack.log").open("wb")

    def start_manager(self) -> None:
        assert self.manager is None
        self.manager = subprocess.Popen(
            [
                str(self.manager_executable),
                "--package-root",
                str(self.package_root),
                "--manager-state-root",
                str(self.manager_root),
                "--runtime-state-root",
                str(self.runtime_root),
                "--logical-scene-database",
                str(self.scene_database),
                "--publication-root",
                str(self.publication_root),
                "--reconciliation-period-ms",
                "20",
            ],
            stdout=self.log,
            stderr=subprocess.STDOUT,
        )

    def start_api(self) -> None:
        assert self.api is None
        self.api = subprocess.Popen(
            [
                sys.executable,
                "-m",
                "manyforge_headless_api",
                "serve",
                "--host",
                "127.0.0.1",
                "--port",
                str(self.port),
                "--publication-root",
                str(self.publication_root),
                "--package-root",
                str(self.package_root),
                "--node-catalog-path",
                str(NODE_CATALOG),
                "--alarm-catalog-path",
                str(CATALOG),
                "--historian-database-path",
                str(self.history_database),
                "--manager-state-root",
                str(self.manager_root),
                "--cell-id",
                "c1_build_authority_cell",
                "--owner-id",
                "headless-api.process-test",
                "--freshness-ms",
                "200",
            ],
            stdout=self.log,
            stderr=subprocess.STDOUT,
            env=os.environ.copy(),
        )

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

    def start_composer(self) -> None:
        assert self.composer is None
        self.composer = subprocess.Popen(
            [
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
                "0.25",
            ],
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
                client.connect(str(self.manager_root / "manager-command.sock"))
                client.sendall(request)
                response = client.recv(16 * 1024)
                if b'"ok":true' not in response:
                    raise RuntimeError("Manager shutdown was rejected")
            self.manager.wait(timeout=5)
        finally:
            if self.manager.poll() is None:
                self.manager.terminate()
                self.manager.wait(timeout=5)
            self.manager = None

    def close(self) -> None:
        try:
            if self.composer is not None and self.composer.poll() is None:
                self.composer.terminate()
                self.composer.wait(timeout=5)
            self.composer = None
        finally:
            try:
                self.private_shutdown_manager()
            finally:
                if self.api is not None and self.api.poll() is None:
                    self.api.terminate()
                    self.api.wait(timeout=5)
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


def _apply_body(owner_epoch: str, request_id: str) -> dict:
    return {
        "request_id": request_id,
        "request_owner_epoch": owner_epoch,
        "source_label": "process_test",
        "correlation_id": "correlation.1",
        "deployment_artifact_id": "candidate",
        "deployment_revision": "sha256:" + "a" * 64,
        "expected_current": {
            "graph_revision": "current-graph",
            "retained_bundle_id": "current-bundle",
        },
        "reject_if_affected_busy": True,
    }


def _owner_identity(runtime: dict, alarms: dict) -> tuple[object, ...]:
    return (
        runtime["projection_owner_epoch"],
        runtime["projection_generation"],
        alarms["owner_epoch"],
        alarms["generation"],
        alarms["state_revision"],
    )


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
                client.manager_apply(_apply_body(first_epoch, "request.1"))
            assert unavailable.value.code == "kernel_owner_unavailable"

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
            "node-environment": "manyforge.node_environment_projection.v1",
            "scene": "manyforge.scene_projection.v1",
            "alarms": "manyforge.operator_alarm_snapshot.v2",
            "history": "manyforge.historian_page.v1",
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
        assert "kernel_owner_unavailable" in apply_command.stderr

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
                client.manager_apply(_apply_body(first_epoch, "request.stale"))
            assert stale.value.code == "stale_request_owner_epoch"

            with pytest.raises(HeadlessClientError) as unavailable:
                client.manager_apply(
                    _apply_body(replacement_epoch, "request.replacement")
                )
            assert unavailable.value.code == "kernel_owner_unavailable"
