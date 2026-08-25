"""Run the bounded I6/N7 maintained native product matrix.

This runner consumes an already generated retained package and already built
executables. Each row crosses the production Manager, native owner, Headless,
Program runner, and thin Composer path. It writes only per-row temporary
state/logs, deletes them on exit, and emits one compact JSON result. Builds and
raw process output are not qualification deliverables.
"""

from __future__ import annotations

import json
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
from urllib.request import Request, urlopen

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_operator.cell_layout import CellInstanceLayout
from websockets.sync.client import connect

WORKSPACE = Path(os.environ.get("MANYFORGE_ROOT", "/workspace")).resolve()
PACKAGE = Path(os.environ["N7_MATRIX_PACKAGE"]).resolve()
INSTALLED = Path(os.environ["N7_MATRIX_INSTALLED"]).resolve()
NODE_CATALOG = WORKSPACE / "manyforge_behavior/resources/node_catalog.yaml"
ALARM_CATALOG = Path(
    WORKSPACE / "manyforge_headless/resources/operator-alarm-catalog.v1.yaml"
)
MODEL_ID = "qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge"
POSE_MODEL_ID = "cosmos-reason2-8b"
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
        raise RuntimeError(
            f"Composer {method} {path} returned {error.code}: {payload}"
        ) from error
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
                raise RuntimeError(
                    f"process exited during startup: {process.returncode}"
                )
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
) -> dict[str, object]:
    deadline = time.monotonic() + 20.0
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError(
                    f"process exited during Composer startup: {process.returncode}"
                )
        try:
            catalog = composer_json(base_url, "GET", "/api/scenarios")
            if catalog.get("api_version") == "manyforge.scenario_catalog.v1":
                return catalog
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
    model_config: Path,
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
    program_cycle_journal = (
        layout.owner_state_root("program-runner") / "program-cycle.sqlite3"
    )
    authoring_state_root = layout.owner_state_root("composer-authoring")
    program_runner_liveness_socket = (
        manager_run_root / "program-runner-liveness.sock"
    )
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
            "--authoring-transaction-journal",
            str(authoring_state_root / "composer-authoring-transactions.sqlite3"),
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
            "--model-endpoint-config",
            str(model_config),
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
            "--program-selection-state-root",
            str(layout.owner_state_root("program-selection")),
            "--node-catalog-path",
            str(NODE_CATALOG),
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
            "--program-path",
            str(PACKAGE / PROGRAM_FILES[scenario_id]),
            "--headless-api-url",
            api_url,
            "--headless-api-timeout-s",
            "15",
            "--authoring-state-root",
            str(authoring_state_root),
        ],
    }


def _authoring_apply_body(
    *,
    workspace: dict[str, object],
    snapshot: dict[str, object],
    client_id: str,
    scenario_id: str,
    scenario_catalog_revision: str,
) -> dict[str, object]:
    draft = workspace.get("draft")
    admission = snapshot.get("authoring_admission")
    cycle = snapshot.get("program_cycle")
    snapshot_draft = snapshot.get("draft_identity")
    receiver_epoch = snapshot.get("receiver_epoch")
    sequence = snapshot.get("snapshot_sequence")
    if (
        workspace.get("schemaVersion")
        != "manyforge.composer_program_workspace.v1"
        or snapshot.get("schema_version")
        != "manyforge.composer_headless_state.v1"
        or not client_id
        or not isinstance(draft, dict)
        or not isinstance(admission, dict)
        or not isinstance(cycle, dict)
        or not isinstance(snapshot_draft, dict)
        or snapshot.get("complete") is not True
        or admission.get("authoring_allowed") is not True
        or cycle.get("state") != "idle_after_reset"
        or snapshot_draft.get("draft_id") != draft.get("draftId")
        or snapshot_draft.get("draft_revision") != draft.get("draftRevision")
        or not isinstance(receiver_epoch, str)
        or not receiver_epoch
        or type(sequence) is not int
        or sequence < 1
        or type(admission.get("admission_generation")) is not int
        or admission["admission_generation"] < 1
    ):
        raise RuntimeError("Composer did not publish one Apply-ready snapshot")
    return {
        "draftId": draft["draftId"],
        "expectedDraftRevision": draft["draftRevision"],
        "clientId": client_id,
        "snapshotReceiverEpoch": receiver_epoch,
        "snapshotSequence": sequence,
        "expectedAdmissionGeneration": admission["admission_generation"],
        "requestOwnerEpoch": "n7-product-matrix.authoring",
        "requestId": f"n7.apply.{scenario_id}",
        "scenarioCatalogRevision": scenario_catalog_revision,
        "scenarioId": scenario_id,
    }


def qualify_scenario(
    scenario_id: str,
    cell_id: str,
    admission_filename: str,
    expected_state: str,
    model_config: Path,
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
                model_config=model_config,
                api_port=api_port,
                runner_port=runner_port,
                composer_port=composer_port,
                layout=layout,
            )
            api_url = f"http://127.0.0.1:{api_port}"
            composer_url = f"http://127.0.0.1:{composer_port}"
            environment = os.environ.copy()
            manager = subprocess.Popen(
                commands["manager"],
                stdout=log,
                stderr=subprocess.STDOUT,
            )
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
            wait_for_api(api_url, (manager, runner, api))
            composer_environment = environment.copy()
            composer_environment["MANYFORGE_API_TOKEN"] = "ignored-legacy-token"
            composer_environment["MANYFORGE_FS_ROOTS"] = str(PACKAGE)
            composer = subprocess.Popen(
                commands["composer"],
                stdout=log,
                stderr=subprocess.STDOUT,
                env=composer_environment,
            )
            composer_catalog = wait_for_composer(
                composer_url,
                (manager, runner, api, composer),
            )
            with HeadlessHttpClient(api_url, timeout_seconds=15.0) as client:
                catalog = client.scenarios()
                if composer_catalog != catalog:
                    raise RuntimeError("Composer projected another scenario catalog")
                program = load_strict_json_yaml(PACKAGE / PROGRAM_FILES[scenario_id])
                workspace = composer_json(
                    composer_url,
                    "GET",
                    "/api/program/workspace",
                )
                browser_client_id = f"n7-product-matrix.{scenario_id}"
                with connect(
                    f"ws://127.0.0.1:{composer_port}/ws/state"
                    f"?client_id={browser_client_id}",
                    open_timeout=5.0,
                    close_timeout=1.0,
                    max_size=16 * 1024 * 1024,
                    proxy=None,
                ) as state_socket:
                    raw_snapshot = state_socket.recv(timeout=5.0)
                    if not isinstance(raw_snapshot, str):
                        raise RuntimeError("Composer state snapshot is not text")
                    snapshot_bytes = raw_snapshot.encode("utf-8")
                    if len(snapshot_bytes) > 16 * 1024 * 1024:
                        raise RuntimeError("Composer state snapshot is oversized")
                    headless_snapshot = json.loads(snapshot_bytes)
                    if not isinstance(headless_snapshot, dict):
                        raise RuntimeError("Composer state snapshot is not an object")
                    apply_body = _authoring_apply_body(
                        workspace=workspace,
                        snapshot=headless_snapshot,
                        client_id=browser_client_id,
                        scenario_id=scenario_id,
                        scenario_catalog_revision=catalog[
                            "scenario_catalog_revision"
                        ],
                    )
                    composer_apply = composer_json(
                        composer_url,
                        "POST",
                        "/api/program/apply",
                        apply_body,
                    )
                applied_result = composer_apply.get("headless")
                if (
                    composer_apply.get("status") != "applied"
                    or not isinstance(applied_result, dict)
                    or applied_result.get("status") != "applied"
                    or not isinstance(applied_result.get("applied"), dict)
                ):
                    raise RuntimeError(
                        f"Composer Apply returned a divergent result: {composer_apply}"
                    )
                applied = applied_result["applied"]
                selection = client.scenario_program_selection(scenario_id)
                if selection["program"] != program:
                    raise RuntimeError("applied scenario selected another Program")
                readiness_deadline = time.monotonic() + 25.0
                while True:
                    try:
                        client.execution_status()
                        break
                    except HeadlessClientError:
                        if time.monotonic() >= readiness_deadline:
                            raise
                        time.sleep(0.02)
                snapshot = composer_json(
                    composer_url,
                    "POST",
                    "/api/program/runs",
                    {
                        "correlation_id": f"n7.{scenario_id}",
                        "expected_applied_record_digest": applied["source"][
                            "record_digest"
                        ],
                        "request_id": f"n7.start.{scenario_id}",
                        "request_owner_epoch": "n7-product-matrix",
                        "scenario_catalog_revision": catalog[
                            "scenario_catalog_revision"
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
                raise RuntimeError(
                    f"{scenario_id} ended as {snapshot['state']}: {snapshot}"
                )
            if (
                expected_state == "failed"
                and snapshot["reason_code"] != EXPECTED_SNAPSHOT_SOURCE_GATE
            ):
                raise RuntimeError(
                    f"{scenario_id} failed for the wrong reason: {snapshot}"
                )
            layout.validate()
            return {
                "elapsed_s": round(time.monotonic() - started_at, 3),
                "reason_code": snapshot["reason_code"],
                "scenario_id": scenario_id,
                "state": snapshot["state"],
            }
        except Exception as error:
            log.flush()
            detail = log_path.read_text(encoding="utf-8", errors="replace")[-12000:]
            raise RuntimeError(
                f"{scenario_id}: {error}\n--- stack log ---\n{detail}"
            ) from error
        finally:
            stop(composer)
            stop(runner)
            stop(api)
            stop(manager)
            log.close()


def main() -> int:
    model_port = free_port()
    server = ThreadingHTTPServer(("127.0.0.1", model_port), ModelHandler)
    thread = Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="n7-model-config-") as raw_root:
            model_config = Path(raw_root) / "site.json"
            model_config.write_text(
                json.dumps(
                    {
                        "api_version": "manyforge.program_model_site_config.v1",
                        "endpoints": [
                            {
                                "bearer_token_env": None,
                                "chat_completions_url": (
                                    f"http://127.0.0.1:{model_port}/v1/chat/completions"
                                ),
                                "external_resource_alias": alias,
                                "headers": {},
                                "model_endpoint_aliases": [alias],
                                "model_ids": (
                                    [MODEL_ID]
                                    if alias == "selection_model"
                                    else [POSE_MODEL_ID, MODEL_ID]
                                ),
                                "tool_call_families": ["openai_chat_tools"],
                            }
                            for alias in ("selection_model", "vision_model")
                        ],
                    },
                    separators=(",", ":"),
                    sort_keys=True,
                ),
                encoding="utf-8",
            )
            selected_id = os.environ.get("N7_MATRIX_SCENARIO")
            rows = tuple(
                row for row in SCENARIOS if selected_id is None or row[0] == selected_id
            )
            if not rows:
                raise RuntimeError("N7_MATRIX_SCENARIO selected no scenario")
            results = [qualify_scenario(*row, model_config) for row in rows]
        expected_model_calls = int(
            any(row[0] == "ur5_semantic_shape_pick" for row in rows)
        )
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
                raise RuntimeError(
                    f"model request was not the qualified tool call: {request}"
                )
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
