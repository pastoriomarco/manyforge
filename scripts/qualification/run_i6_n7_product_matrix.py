"""Run the bounded I6/N7 maintained native product matrix.

This runner consumes an already generated retained package and already built
executables. Each row crosses the production Manager, native owner, Headless,
Program runner, and thin Composer path. It writes only per-row temporary
state/logs, deletes them on exit, and emits one compact JSON result. Builds and
raw process output are not qualification deliverables.
"""

from __future__ import annotations

import json
import importlib.util
import os
import socket
import subprocess
import sys
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from urllib.error import HTTPError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import TargetObservationQuery
from manyforge_operator.cell_layout import CellInstanceLayout

WORKSPACE = Path(os.environ.get("MANYFORGE_ROOT", "/workspace")).resolve()
PACKAGE = Path(os.environ["N7_MATRIX_PACKAGE"]).resolve()
INSTALLED = Path(os.environ["N7_MATRIX_INSTALLED"]).resolve()
NODE_CATALOG = WORKSPACE / "manyforge_behavior/resources/node_catalog.yaml"
ALARM_CATALOG = Path(WORKSPACE / "manyforge_headless/resources/operator-alarm-catalog.v1.yaml")
MODEL_ID = "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge"
EXPECTED_SNAPSHOT_SOURCE_GATE = "model_snapshot_image_source_unavailable"

SCENARIOS = (
    (
        "lite6_pick_and_place",
        "lite6_pick_place_cell",
        "canonical-empty-admission.lite6_pick_place_cell.json",
        "succeeded",
    ),
    (
        "ur5_pick_and_place",
        "ur5_pick_place_cell",
        "canonical-empty-admission.ur5_pick_place_cell.json",
        "succeeded",
    ),
    (
        "ur5_pick_and_place_minimal",
        "ur5_pick_place_cell",
        "canonical-empty-admission.ur5_pick_place_cell.json",
        "succeeded",
    ),
    (
        "ur10e_pick_and_place",
        "ur10e_pick_place_cell",
        "canonical-empty-admission.ur10e_pick_place_cell.json",
        "succeeded",
    ),
    (
        "ur10e_assistant_scene_authoring",
        "ur10e_assistant_authoring_cell",
        "canonical-empty-admission.ur10e_assistant_authoring_cell.json",
        "succeeded",
    ),
    (
        "ur5_semantic_shape_pick",
        "ur5_semantic_shape_pick_cell",
        "canonical-empty-admission.ur5_semantic_shape_pick_cell.json",
        "succeeded",
    ),
    (
        "ur5_vision_guided_bin_pick",
        "ur5_vision_bin_pick_cell",
        "canonical-empty-admission.ur5_vision_bin_pick_cell.json",
        "failed",
    ),
    (
        "ur5_vision_guided_pose_pick",
        "ur5_vision_pose_pick_cell",
        "canonical-empty-admission.ur5_vision_pose_pick_cell.json",
        "failed",
    ),
)

PROGRAM_FILES = {
    "lite6_pick_and_place": "lite6_lite_gripper_pick_and_place.program.yaml",
    "ur5_pick_and_place": "pick_and_place.program.yaml",
    "ur5_pick_and_place_minimal": "pick_and_place_minimal.program.yaml",
    "ur10e_pick_and_place": "pick_and_place_ur10e_robotiq.program.yaml",
    "ur10e_assistant_scene_authoring": "pick_and_place_ur10e_robotiq.program.yaml",
    "ur5_semantic_shape_pick": "semantic_shape_pick.program.yaml",
    "ur5_vision_guided_bin_pick": "vision_guided_bin_pick.program.yaml",
    "ur5_vision_guided_pose_pick": "vision_guided_pose_pick.program.yaml",
}


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def composer_json(
    base_url: str,
    method: str,
    path: str,
    body: dict[str, object] | None = None,
) -> dict[str, object]:
    encoded = None
    headers: dict[str, str] = {}
    if body is not None:
        encoded = json.dumps(body, separators=(",", ":"), sort_keys=True).encode()
        headers["Content-Type"] = "application/json"
    request = Request(
        f"{base_url}{path}",
        data=encoded,
        headers=headers,
        method=method,
    )
    try:
        with urlopen(request, timeout=15.0) as response:  # noqa: S310
            payload = response.read()
    except HTTPError as error:
        payload = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"Composer {method} {path} returned {error.code}: {payload}") from error
    decoded = json.loads(payload)
    if not isinstance(decoded, dict):
        raise RuntimeError(f"Composer {method} {path} returned a non-object")
    return decoded


model_requests: list[dict[str, object]] = []


class ModelHandler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:  # noqa: N802
        size = int(self.headers.get("Content-Length", "0"))
        request = json.loads(self.rfile.read(size))
        model_requests.append(request)
        arguments = {
            "confidence": 0.95,
            "matched_attributes": ["shape=sphere"],
            "outcome": "selected",
            "reason": "the request selects the declared sphere",
            "selected_object_id": "sphere_part",
        }
        response = json.dumps(
            {
                "choices": [
                    {
                        "message": {
                            "tool_calls": [
                                {
                                    "function": {
                                        "arguments": json.dumps(
                                            arguments,
                                            separators=(",", ":"),
                                            sort_keys=True,
                                        ),
                                        "name": "select_scene_object",
                                    }
                                }
                            ]
                        }
                    }
                ],
                "model": MODEL_ID,
                "usage": {"total_tokens": 17},
            },
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, _format: str, *_args: object) -> None:
        return


def wait_for_api(base_url: str, processes: tuple[subprocess.Popen[bytes], ...]) -> None:
    deadline = time.monotonic() + 20.0
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError(f"process exited during startup: {process.returncode}")
        try:
            with HeadlessHttpClient(base_url, timeout_seconds=1.0) as client:
                runtime = client.runtime()
            if runtime.get("complete") is True:
                return
        except Exception as error:
            last_error = error
        time.sleep(0.02)
    raise RuntimeError(f"headless stack did not become complete: {last_error}")


def wait_for_composer(
    base_url: str,
    processes: tuple[subprocess.Popen[bytes], ...],
) -> None:
    deadline = time.monotonic() + 20.0
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError(f"process exited during Composer startup: {process.returncode}")
        try:
            health = composer_json(base_url, "GET", "/api/health")
            if isinstance(health.get("status"), str):
                return
        except Exception as error:
            last_error = error
        time.sleep(0.02)
    raise RuntimeError(f"Composer did not become ready: {last_error}")


def stop(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def _process_commands(
    *,
    scenario_id: str,
    cell_id: str,
    admission_filename: str,
    api_port: int,
    runner_port: int,
    composer_port: int,
    layout: CellInstanceLayout,
) -> dict[str, list[str]]:
    api_url = f"http://127.0.0.1:{api_port}"
    manager_state_root = layout.owner_state_root("manager")
    manager_run_root = layout.run_root / "manager"
    execution_state_root = layout.owner_state_root("execution-host")
    execution_run_root = layout.run_root / "execution-host"
    program_cycle_journal = layout.owner_state_root("program-runner") / "program-cycle.sqlite3"
    program_runner_liveness_socket = manager_run_root / "program-runner-liveness.sock"
    return {
        "manager": [
            str(INSTALLED / "manyforge_manager_daemon"),
            "--install-root",
            str(PACKAGE),
            "--canonical-empty-admission-filename",
            admission_filename,
            "--manager-state-root",
            str(manager_state_root),
            "--manager-run-root",
            str(manager_run_root),
            "--execution-state-root",
            str(execution_state_root),
            "--execution-run-root",
            str(execution_run_root),
            "--logical-scene-database",
            str(layout.owner_state_root("logical-scene") / "scene.sqlite3"),
            "--program-cycle-journal",
            str(program_cycle_journal),
            "--projection-root",
            str(layout.projection_root),
            "--program-runner-liveness-socket",
            str(program_runner_liveness_socket),
            "--reconciliation-period-ms",
            "20",
        ],
        "program": [
            sys.executable,
            "-m",
            "manyforge_program_runner",
            "--host",
            "127.0.0.1",
            "--port",
            str(runner_port),
            "--headless-url",
            api_url,
            "--owner-id",
            f"program-runner.{scenario_id}",
            "--headless-timeout-s",
            "10",
            "--execution-run-root",
            str(execution_run_root),
            "--cycle-journal-path",
            str(program_cycle_journal),
            "--manager-liveness-socket",
            str(program_runner_liveness_socket),
            "--cell-id",
            cell_id,
        ],
        "headless": [
            sys.executable,
            "-m",
            "manyforge_headless_api",
            "serve",
            "--host",
            "127.0.0.1",
            "--port",
            str(api_port),
            "--projection-root",
            str(layout.projection_root),
            "--install-root",
            str(PACKAGE),
            "--artifact-root",
            str(layout.artifact_root),
            "--node-catalog-path",
            str(NODE_CATALOG),
            "--node-environment-manifest-path",
            str(WORKSPACE / "manyforge_behavior/resources/node_environment_manifest.yaml"),
            "--alarm-catalog-path",
            str(ALARM_CATALOG),
            "--historian-database-path",
            str(layout.owner_state_root("alarm-history") / "history.sqlite3"),
            "--manager-run-root",
            str(manager_run_root),
            "--execution-run-root",
            str(execution_run_root),
            "--cell-id",
            cell_id,
            "--owner-id",
            f"headless-api.{scenario_id}",
            "--freshness-ms",
            "120000",
            "--manager-timeout-s",
            "5",
            "--program-runner-url",
            f"http://127.0.0.1:{runner_port}",
            "--program-runner-timeout-s",
            "10",
        ],
        "composer": [
            sys.executable,
            "-m",
            "manyforge_composer",
            "--host",
            "127.0.0.1",
            "--port",
            str(composer_port),
            "--startup-scenario-document",
            str(WORKSPACE / "examples" / f"{scenario_id}.scenario"),
            "--headless-api-url",
            api_url,
            "--headless-api-timeout-s",
            "15",
        ],
}


def qualify_scenario(
    scenario_id: str,
    cell_id: str,
    admission_filename: str,
    expected_state: str,
) -> dict[str, object]:
    started_at = time.monotonic()
    with tempfile.TemporaryDirectory(prefix=f"n7-{scenario_id}-") as raw_root:
        root = Path(raw_root).resolve()
        layout = CellInstanceLayout.open_or_create(
            cell_id=cell_id,
            cell_root=root,
            install_root=PACKAGE,
        )
        log_path = layout.run_root / "operator" / "stack.log"
        log = log_path.open("wb")
        manager: subprocess.Popen[bytes] | None = None
        api: subprocess.Popen[bytes] | None = None
        runner: subprocess.Popen[bytes] | None = None
        composer: subprocess.Popen[bytes] | None = None
        try:
            api_port = free_port()
            runner_port = free_port()
            composer_port = free_port()
            commands = _process_commands(
                scenario_id=scenario_id,
                cell_id=cell_id,
                admission_filename=admission_filename,
                api_port=api_port,
                runner_port=runner_port,
                composer_port=composer_port,
                layout=layout,
            )
            api_url = f"http://127.0.0.1:{api_port}"
            composer_url = f"http://127.0.0.1:{composer_port}"
            environment = os.environ.copy()
            runner = subprocess.Popen(
                commands["program"],
                stdout=log,
                stderr=subprocess.STDOUT,
                env=environment,
            )
            api = subprocess.Popen(
                commands["headless"],
                stdout=log,
                stderr=subprocess.STDOUT,
                env=environment,
            )
            manager = subprocess.Popen(
                commands["manager"],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            wait_for_api(api_url, (manager, runner, api))
            composer_environment = environment.copy()
            composer_environment["MANYFORGE_API_TOKEN"] = "ignored-legacy-token"
            composer_environment["MANYFORGE_FS_ROOTS"] = os.pathsep.join(
                (str(PACKAGE), str(WORKSPACE))
            )
            composer = subprocess.Popen(
                commands["composer"],
                stdout=log,
                stderr=subprocess.STDOUT,
                env=composer_environment,
            )
            wait_for_composer(
                composer_url,
                (manager, runner, api, composer),
            )
            with HeadlessHttpClient(api_url, timeout_seconds=15.0) as client:
                program = load_strict_json_yaml(PACKAGE / PROGRAM_FILES[scenario_id])
                applied = client.program_applied()
                if applied.get("scenario_id") != scenario_id:
                    raise RuntimeError(
                        f"project startup activated another scenario: {applied}"
                    )
                if applied["program"] != program:
                    raise RuntimeError("applied scenario selected another Program")
                # Use the maintained operator's exact CCLC convergence, not a
                # second lifecycle implementation or diagnostics-as-readiness.
                spec = importlib.util.spec_from_file_location(
                    "matrix_workbench", WORKSPACE / "scripts/dev-workbench.py"
                )
                assert spec is not None and spec.loader is not None
                workbench = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(workbench)
                workbench.PACKAGE_ROOT = PACKAGE
                metadata = workbench._load_scenario(scenario_id)
                workbench._converge_control_management(
                    client,
                    metadata["component_ids"],
                    scenario_id=scenario_id,
                    processes=[
                        ("manager", manager),
                        ("runner", runner),
                        ("headless", api),
                        ("composer", composer),
                    ],
                )
                # Read the real feedback binding before any effect. UR5 has a
                # component-local gripper coordinate; Lite6/UR10e use model
                # coordinates. Neither kind of read needs command authority.
                native = ExecutionCommandClient(layout.run_root / "execution-host")
                owner_epoch = native.identity()["identity"]["owner_epoch"]
                gripper_queries = []
                for role_id, role in program["target_roles"].items():
                    if not any(
                        capability["capability_id"] == "tool.gripper.command"
                        for capability in role["required_capabilities"]
                    ):
                        continue
                    feedback = native.query_target(
                        TargetObservationQuery(
                            schema_version="manyforge.target_observation_query.v1",
                            owner_epoch=owner_epoch,
                            program_binding_id=metadata["program_binding_id"],
                            target_role_id=role_id,
                            kind="gripper_status",
                        )
                    )["target_observation"]
                    if feedback["value"]["grasp_confirmed"] is not None:
                        raise RuntimeError("position feedback invented grasp evidence")
                    gripper_queries.append(role_id)
                admission = client.authoring_admission()
                if admission.get("authoring_allowed") is not True:
                    raise RuntimeError(f"Program admission is not ready: {admission}")
                snapshot = composer_json(
                    composer_url,
                    "POST",
                    "/api/program/runs",
                    {
                        "correlation_id": f"n7.{scenario_id}",
                        "expected_applied_record_digest": applied["source"]["record_digest"],
                        "draft_program_revision": program["program_revision"],
                        "request_id": f"n7.start.{scenario_id}",
                        "request_owner_epoch": "n7-product-matrix",
                        "source_document_digest": applied[
                            "source_document_digest"
                        ],
                        "scenario_id": scenario_id,
                        "source_label": "n7_product_matrix",
                    },
                )
                program_run_id = snapshot["capture"]["program_run_id"]
                run_deadline = time.monotonic() + 50.0
                while snapshot["state"] in {
                    "running",
                    "stopping",
                    "finalizing",
                }:
                    if time.monotonic() >= run_deadline:
                        raise RuntimeError(f"Program run did not settle: {snapshot}")
                    time.sleep(0.02)
                    snapshot = composer_json(
                        composer_url,
                        "GET",
                        f"/api/program/runs/{program_run_id}",
                    )
            if snapshot["state"] != expected_state:
                raise RuntimeError(f"{scenario_id} ended as {snapshot['state']}: {snapshot}")
            if (
                expected_state == "failed"
                and snapshot["reason_code"] != EXPECTED_SNAPSHOT_SOURCE_GATE
            ):
                raise RuntimeError(f"{scenario_id} failed for the wrong reason: {snapshot}")
            layout.validate()
            return {
                "elapsed_s": round(time.monotonic() - started_at, 3),
                "reason_code": snapshot["reason_code"],
                "scenario_id": scenario_id,
                "state": snapshot["state"],
                "gripper_feedback_targets": gripper_queries,
            }
        except Exception as error:
            log.flush()
            detail = log_path.read_text(encoding="utf-8", errors="replace")
            if len(detail) > 28000:
                detail = detail[:16000] + "\n[bounded log excerpt]\n" + detail[-12000:]
            raise RuntimeError(f"{scenario_id}: {error}\n--- stack log ---\n{detail}") from error
        finally:
            stop(composer)
            stop(runner)
            stop(api)
            stop(manager)
            log.close()


def _ai_model_port() -> int:
    """Port of the semantic Scenario's selection AI model component (MODEL_ID).

    The fake serves it; the Scenario's chat component is not called.
    """

    deployment = load_strict_json_yaml(
        WORKSPACE / "examples/ur5_semantic_shape_pick.scenario/deployment.yaml"
    )
    (base_url,) = (
        component["configuration"]["runtime_configuration"]["base_url"]
        for component in deployment["components"]
        if component["configuration"].get("runtime_kind") == "ai_model"
        and component["configuration"]["runtime_configuration"]["model"] == MODEL_ID
    )
    port = urlsplit(base_url).port
    if port is None:
        raise RuntimeError(f"AI model base_url has no explicit port: {base_url}")
    return port


def main() -> int:
    server = ThreadingHTTPServer(("127.0.0.1", _ai_model_port()), ModelHandler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        selected_id = os.environ.get("N7_MATRIX_SCENARIO")
        rows = tuple(row for row in SCENARIOS if selected_id is None or row[0] == selected_id)
        if not rows:
            raise RuntimeError("N7_MATRIX_SCENARIO selected no scenario")
        results = [qualify_scenario(*row) for row in rows]
        expected_model_calls = int(any(row[0] == "ur5_semantic_shape_pick" for row in rows))
        if len(model_requests) != expected_model_calls:
            raise RuntimeError(
                "unexpected bounded semantic-selection call count: "
                f"expected {expected_model_calls}, got {len(model_requests)}"
            )
        if model_requests:
            request = model_requests[0]
            tools = request.get("tools")
            if (
                request.get("model") != MODEL_ID
                or not isinstance(tools, list)
                or len(tools) != 1
                or tools[0].get("function", {}).get("name") != "select_scene_object"
            ):
                raise RuntimeError(f"model request was not the qualified tool call: {request}")
        print(
            json.dumps(
                {"model_calls": len(model_requests), "results": results},
                indent=2,
                sort_keys=True,
            )
        )
        return 0
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=5)


if __name__ == "__main__":
    raise SystemExit(main())
