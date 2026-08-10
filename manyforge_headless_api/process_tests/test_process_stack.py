"""Process-boundary H0 qualification for Manager, API, CLI, and Composer."""

from __future__ import annotations

from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
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
    def __init__(
        self,
        root: Path,
        *,
        package_root: Path | None = None,
        cell_id: str = "c1_build_authority_cell",
        program_path: Path | None = None,
        freshness_milliseconds: int = 200,
    ) -> None:
        self.root = root
        self.manager_root = root / "manager"
        self.execution_root = root / "execution"
        self.runtime_root = root / "runtime"
        self.publication_root = root / "publications"
        for path in (
            self.manager_root,
            self.execution_root,
            self.runtime_root,
            self.publication_root,
        ):
            path.mkdir()
        self.scene_database = root / "logical-scene.sqlite3"
        self.history_database = root / "history.sqlite3"
        self.manager_executable = _required_path("MANYFORGE_MANAGER_DAEMON")
        self.package_root = package_root or _required_path(
            "MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT"
        )
        if not self.package_root.exists():
            raise RuntimeError(f"retained package root does not exist: {self.package_root}")
        self.cell_id = cell_id
        self.program_path = program_path
        self.freshness_milliseconds = freshness_milliseconds
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
                "--execution-state-root",
                str(self.execution_root),
                "--cell-id",
                self.cell_id,
                "--owner-id",
                "headless-api.process-test",
                "--freshness-ms",
                str(self.freshness_milliseconds),
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

    def publish_assistant_source(self) -> None:
        publisher = _required_path("MANYFORGE_ASSISTANT_SOURCE_PUBLISHER")
        subprocess.run(
            [
                str(publisher),
                "--publication-root",
                str(self.publication_root),
            ],
            check=True,
            capture_output=True,
            timeout=10,
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
            "0.25",
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


@contextmanager
def _assistant_stack(root: Path) -> Iterator[ProcessStack]:
    stack = ProcessStack(
        root,
        package_root=_required_path("MANYFORGE_ASSISTANT_AUTHORING_PACKAGE_ROOT"),
        cell_id="i6_two_lite6_controllable_cell",
        program_path=Path(
            "/workspace/examples/pick_and_place_ur10e_robotiq.program.yaml"
        ),
        freshness_milliseconds=120000,
    )
    try:
        # Operational Apply remains unavailable in N6. Publish one bounded,
        # production-shaped Manager source from the independently verified
        # test bundle so this test qualifies the real API/Composer authoring
        # path without claiming a product deployment or runtime effect.
        stack.publish_assistant_source()
        stack.start_api()
        stack.wait_manager_source()
        stack.start_composer()
        stack.wait_composer()
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


def _assistant_callback_body(
    *,
    draft: dict,
    node_environment: dict,
    source_model: dict,
    assistant_mode: dict,
    scene_resource_id: str,
    request_id: str,
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
        "requestId": request_id,
        "requestOwnerEpoch": "assistant.process.epoch.1",
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


def _plain_package_files(root: Path) -> dict[str, tuple[int, int]]:
    return {
        str(path.relative_to(root)): (path.stat().st_size, path.stat().st_mtime_ns)
        for path in sorted(root.rglob("*"))
        if path.is_file() and not path.is_symlink()
    }


def _sole_retained_program(package_root: Path) -> dict:
    candidates = sorted((package_root / "programs" / "sha256").glob("*.json"))
    assert len(candidates) == 1
    value = json.loads(candidates[0].read_text(encoding="utf-8"))
    assert isinstance(value, dict)
    return value


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
        with HeadlessHttpClient(stack.composer_url) as composer:
            node_environment = composer.node_environment()
            source_model = composer.source_model()
            assistant_mode = composer.assistant_mode("composer-assistant")
            draft = composer.request("GET", "/api/program/draft")
            manager_before = composer.manager_status()["status"]
            runtime_before = composer.runtime()
            package_before = _plain_package_files(stack.package_root)

            assert {
                row["model_instance_id"] for row in source_model["occurrences"]
            } == {"lite6_fixture_instance", "lite6_fixture_instance_2"}
            assert {
                row["component_id"] for row in source_model["targets"]
            } == {"lite6_arm", "lite6_arm_2"}
            assert {row["tool_id"] for row in assistant_mode["tools"]} == {
                "program_read",
                "scene_draft_add_object",
            }

            accepted_body = _assistant_callback_body(
                draft=draft,
                node_environment=node_environment,
                source_model=source_model,
                assistant_mode=assistant_mode,
                scene_resource_id="assistant_workpiece",
                request_id="assistant.process.accepted",
            )
            accepted = composer.request(
                "POST",
                "/api/assistant/bridge/tools/scene_draft_add_object",
                body=accepted_body,
            )
            assert accepted["success"] is True
            assert accepted["draftMutated"] is True

            after_accepted = composer.request("GET", "/api/program/draft")
            assert after_accepted["draft_revision"] == draft["draft_revision"] + 1
            assert after_accepted["saved"] is False
            assert after_accepted["applied"] is False
            assert "assistant_workpiece" in {
                row["scene_resource_id"]
                for row in after_accepted["program"]["scene"]
            }

            stale_identity = _assistant_callback_body(
                draft=after_accepted,
                node_environment=node_environment,
                source_model=source_model,
                assistant_mode=assistant_mode,
                scene_resource_id="stale_identity_object",
                request_id="assistant.process.stale-identity",
            )
            stale_identity["catalogHash"] = "sha256:" + "0" * 64
            with pytest.raises(HeadlessClientError) as stale:
                composer.request(
                    "POST",
                    "/api/assistant/bridge/tools/scene_draft_add_object",
                    body=stale_identity,
                )
            assert stale.value.code == "assistant_callback_context_stale"

            invalid_edit = _assistant_callback_body(
                draft=after_accepted,
                node_environment=node_environment,
                source_model=source_model,
                assistant_mode=assistant_mode,
                scene_resource_id="invalid_edit_object",
                request_id="assistant.process.invalid-edit",
            )
            resource = invalid_edit["arguments"]["scene_resource"]
            resource["objectId"] = resource.pop("scene_resource_id")
            rejected = composer.request(
                "POST",
                "/api/assistant/bridge/tools/scene_draft_add_object",
                body=invalid_edit,
            )
            assert rejected["success"] is False
            assert rejected["draftMutated"] is False
            assert rejected["result"]["kind"] == "validation_error"
            assert composer.request("GET", "/api/program/draft") == after_accepted

            program_read = next(
                row
                for row in assistant_mode["tools"]
                if row["tool_id"] == "program_read"
            )
            read_body = _assistant_callback_body(
                draft=after_accepted,
                node_environment=node_environment,
                source_model=source_model,
                assistant_mode=assistant_mode,
                scene_resource_id="unused_for_program_read",
                request_id="assistant.process.read",
            )
            read_body["inputSchemaHash"] = program_read["input_schema_hash"]
            read_body["arguments"] = {}
            callback_latencies_s: list[float] = []
            for index in range(12):
                read_body["requestId"] = f"assistant.process.read.{index}"
                started = time.perf_counter()
                read_result = composer.request(
                    "POST",
                    "/api/assistant/bridge/tools/program_read",
                    body=read_body,
                )
                callback_latencies_s.append(time.perf_counter() - started)
                assert read_result["success"] is True
                assert read_result["draftMutated"] is False
            assert max(callback_latencies_s) < 0.5, callback_latencies_s

        concurrent_bodies = [
            _assistant_callback_body(
                draft=after_accepted,
                node_environment=node_environment,
                source_model=source_model,
                assistant_mode=assistant_mode,
                scene_resource_id=f"concurrent_object_{index}",
                request_id=f"assistant.process.concurrent.{index}",
            )
            for index in range(2)
        ]

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

        with ThreadPoolExecutor(max_workers=2) as executor:
            concurrent_results = list(executor.map(invoke, concurrent_bodies))
        assert sum(kind == "accepted" for kind, _detail in concurrent_results) == 1
        assert [
            detail
            for kind, detail in concurrent_results
            if kind == "rejected"
        ][0] in {
            "assistant_callback_context_stale",
            "composer_draft_revision_stale",
        }

        with HeadlessHttpClient(stack.composer_url) as composer:
            final_draft = composer.request("GET", "/api/program/draft")
            concurrent_ids = {
                row["scene_resource_id"]
                for row in final_draft["program"]["scene"]
                if row["scene_resource_id"].startswith("concurrent_object_")
            }
            assert len(concurrent_ids) == 1
            assert final_draft["draft_revision"] == after_accepted["draft_revision"] + 1
            assert final_draft["saved"] is False
            assert final_draft["applied"] is False
            assert composer.manager_status()["status"] == manager_before
            runtime_after = composer.runtime()
            assert runtime_after["active_retained_bundle_id"] == runtime_before[
                "active_retained_bundle_id"
            ]
            assert runtime_after["active_graph_revision"] == runtime_before[
                "active_graph_revision"
            ]
            assert runtime_after["projection_generation"] == runtime_before[
                "projection_generation"
            ]
            assert _plain_package_files(stack.package_root) == package_before


def test_real_program_apply_reuses_active_deployment_and_survives_api_restart(
    tmp_path: Path,
) -> None:
    with _assistant_stack(tmp_path) as stack:
        assert stack.wait_complete(True)["complete"] is True
        package_before = _plain_package_files(stack.package_root)
        program = _sole_retained_program(stack.package_root)

        with HeadlessHttpClient(stack.base_url) as client:
            scenarios = client.scenarios()
            assert [row["scenario_id"] for row in scenarios["scenarios"]] == [
                "program_apply_fixture"
            ]
            with pytest.raises(HeadlessClientError) as absent:
                client.program_applied()
            assert absent.value.code == "program_applied_selection_not_found"

            result = client.program_apply(
                {
                    "scenario_id": "program_apply_fixture",
                    "scenario_catalog_revision": scenarios[
                        "scenario_catalog_revision"
                    ],
                    "program": program,
                    "expected_applied_record_digest": None,
                }
            )
            assert result["status"] == "applied"
            assert result["deployment_apply_required"] is False
            assert result["manager_response"] is None
            assert result["applied"]["program_revision"] == program[
                "program_revision"
            ]
            first_applied = client.program_applied()
            assert first_applied == result["applied"]

        stack.kill_api()
        stack.start_api()
        stack.wait_complete(True)
        with HeadlessHttpClient(stack.base_url) as restarted:
            reopened = restarted.program_applied()
            assert reopened == first_applied
            replay = restarted.program_apply(
                {
                    "scenario_id": "program_apply_fixture",
                    "scenario_catalog_revision": scenarios[
                        "scenario_catalog_revision"
                    ],
                    "program": program,
                    "expected_applied_record_digest": first_applied["source"][
                        "record_digest"
                    ],
                }
            )
            assert replay["applied"] == first_applied
        assert _plain_package_files(stack.package_root) == package_before


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
            "source-model": "manyforge.source_model_projection.v1",
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
