from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any, Iterator

import httpx
import pytest
from fastapi import WebSocketDisconnect
from fastapi.testclient import TestClient

from manyforge_behavior.canonical_artifact import (
    CONTENT_IDENTITY_PREFIX,
    canonical_json_bytes,
    compute_content_identity,
    raw_sha256_digest,
)
from manyforge_behavior.executable_node_resolver import build_executable_node_resolver
from manyforge_behavior.node_environment import build_node_environment
from manyforge_composer.backend import headless_ws
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.headless_ws import serve_headless_state
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


def _runtime() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.headless_runtime_snapshot.v1",
        "complete": True,
        "projection_owner_epoch": "projection.epoch.1",
        "projection_generation": 4,
    }


def _alarms() -> dict[str, Any]:
    return {
        "schema_version": "manyforge.operator_alarm_snapshot.v2",
        "owner_epoch": "alarm.epoch.1",
        "generation": 3,
        "state_revision": 5,
        "alarms": [],
        "unacknowledged_membership": [],
    }


def _scene_body(
    *,
    request_id: str = "request.scene.1",
    scene_resource_id: str = "part.1",
) -> dict[str, Any]:
    scene_id = "sha256:" + "d" * 64
    return {
        "request_context": {
            "request_id": request_id,
            "request_owner_epoch": "scene-owner.epoch.7",
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "expected_projection_revision": {
            "retained_bundle_id": "sha256:" + "b" * 64,
            "graph_revision": "sha256:" + "c" * 64,
            "scene_revision": {
                "scene_id": scene_id,
                "revision_id": 1,
                "content_hash": "sha256:" + "a" * 64,
            },
        },
        "operation": {
            "kind": "remove_object",
            "scene_resource_id": scene_resource_id,
        },
    }


def _program_run_status(*, state: str = "running") -> dict[str, Any]:
    digest_a = "sha256:" + "a" * 64
    digest_b = "sha256:" + "b" * 64
    digest_c = "sha256:" + "c" * 64
    return {
        "schema_version": "manyforge.program_run_status.v1",
        "owner_id": "program-runner.cell-a",
        "owner_epoch": "program-runner.epoch.1",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v1",
            "program_run_id": "program-run.1",
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": digest_a,
            "program_id": "pick",
            "program_revision": digest_b,
            "program_binding_id": digest_c,
            "retained_bundle_id": digest_a,
            "graph_revision": digest_b,
            "node_environment_digest": digest_c,
            "owner_identities": [
                {
                    "owner_role": "program_runner",
                    "owner_id": "program-runner.cell-a",
                    "owner_epoch": "program-runner.epoch.1",
                }
            ],
        },
        "tree_status": "running",
        "active_operation_count": 1,
        "reason_code": None,
        "message": "",
    }


@pytest.fixture
def headless_client() -> (
    Iterator[tuple[HeadlessHttpClient, list[dict[str, Any]], dict[str, Any]]]
):
    requests: list[dict[str, Any]] = []
    environment = build_node_environment()
    manifest = environment.manifest
    dispositions = {
        item.row: item
        for item in build_executable_node_resolver(environment).dispositions
    }
    node_environment = {
        "schema_version": "manyforge.node_environment_projection.v1",
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "catalog_id": manifest.catalog_id,
        "content_version": manifest.catalog_content_version,
        "catalog_digest": manifest.catalog_digest,
        "environment_digest": manifest.environment_digest,
        "catalog": environment.catalog.raw_data,
        "manifest": manifest.to_dict(),
        "availability": {
            "binding_context": "unselected",
            "execution_available": False,
            "implementations": [
                {
                    "adapter_build_qualified": dispositions[
                        row
                    ].adapter_build_qualified,
                    "authoring_available": True,
                    "binding_available": False,
                    "engine_id": row.engine_id,
                    "executable_package_id": row.executable_package_id,
                    "execution_available": False,
                    "implementation_id": row.implementation_id,
                    "implementation_identity_resolved": True,
                    "node_id": row.node_id,
                    "owner_health_available": (
                        dispositions[row].adapter_build_qualified
                        and dispositions[row].owner_port_name is None
                    ),
                    "reason_codes": sorted(
                        {
                            "program_binding_unselected",
                            *(
                                [dispositions[row].unavailable_reason_code]
                                if dispositions[row].unavailable_reason_code is not None
                                else []
                            ),
                            *(
                                ["program_runtime_owner_unavailable"]
                                if dispositions[row].owner_port_name is not None
                                or not dispositions[row].adapter_build_qualified
                                else []
                            ),
                        }
                    ),
                    "restoration_gates": list(dispositions[row].restoration_gates),
                }
                for row in manifest.executable_registry
            ],
            "manager_runtime_converged": True,
            "portable_authoring_available": True,
            "reason_codes": [
                "program_binding_unselected",
                "program_runtime_owner_unavailable",
            ],
            "restoration_gates": ["I6", "N"],
        },
        "source": {
            "projection_generation": 4,
            "projection_owner_epoch": "projection.epoch.1",
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
            "receiver_age_ns": 10,
            "source_generation": 2,
            "source_owner_epoch": "manager.epoch.1",
            "source_owner_id": "component-manager.cell-a",
            "source_content_digest": "sha256:" + "a" * 64,
            "selection_projection_generation": 4,
            "selection_source_content_digest": "sha256:" + "a" * 64,
            "selection_source_generation": 2,
            "selection_source_owner_epoch": "manager.epoch.1",
            "selection_source_owner_id": "component-manager.cell-a",
        },
    }
    source_model = {
        "schema_version": "manyforge.source_model_projection.v1",
        "cell_id": "cell-a",
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "model_map_contract": None,
        "model_map_product_id": None,
        "occurrences": [],
        "targets": [],
        "source": {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
            "projection_generation": 1,
            "projection_owner_epoch": "source-model-projection.epoch.1",
            "receiver_age_ns": 10,
            "selection_projection_generation": 4,
            "selection_source_content_digest": "sha256:" + "a" * 64,
            "selection_source_generation": 2,
            "selection_source_owner_epoch": "manager.epoch.1",
            "selection_source_owner_id": "component-manager.cell-a",
            "source_content_digest": "",
            "source_generation": 1,
            "source_owner_epoch": "source-model.epoch.1",
            "source_owner_id": "headless-api.source-model",
            "source_owner_kind": "source_model_projection",
        },
    }
    source_model["source"]["source_content_digest"] = raw_sha256_digest(
        b"\0".join(
            (
                CONTENT_IDENTITY_PREFIX,
                b"manyforge-source-model-projection-semantic",
                b"manyforge.source_model_projection.v1",
                canonical_json_bytes(
                    {key: value for key, value in source_model.items() if key != "source"}
                ),
            )
        )
    )
    source_model_context = {
        "schema_version": "manyforge.source_model_context.v1",
        "node_environment": json.loads(json.dumps(node_environment)),
        "source_model": json.loads(json.dumps(source_model)),
    }
    # Keep the legacy standalone fakes on opposite generations. Composer's
    # source-model route must consume only the coherent context above.
    source_model["source"]["selection_projection_generation"] = 5
    applied_program: dict[str, Any] | None = None

    def handler(request: httpx.Request) -> httpx.Response:
        nonlocal applied_program
        body = json.loads(request.content) if request.content else None
        requests.append(
            {
                "method": request.method,
                "path": request.url.path,
                "query": request.url.query.decode("ascii"),
                "body": body,
            }
        )
        path = request.url.path
        if path == "/api/health":
            payload = {
                "schema_version": "manyforge.headless_health.v1",
                "status_scope": "headless_projection",
                "status": "degraded",
                "headless_projection_ready": False,
                "robot_readiness": {
                    "available": False,
                    "state": None,
                    "source_owner_id": None,
                    "reason_code": "robot_readiness_owner_unavailable",
                },
            }
        elif path == "/api/runtime":
            payload = _runtime()
        elif path == "/api/manager/status":
            payload = {"schema_version": "manyforge.manager_status_projection.v1"}
        elif path == "/api/manager/graph":
            payload = {"schema_version": "manyforge.manager_graph_projection.v1"}
        elif path == "/api/manager/diff":
            payload = {"schema_version": "manyforge.manager_diff_projection.v1"}
        elif path == "/api/node-environment":
            payload = node_environment
        elif path == "/api/source-model":
            payload = source_model
        elif path == "/api/source-model-context":
            payload = source_model_context
        elif path == "/api/scenarios":
            payload = {
                "api_version": "manyforge.scenario_catalog.v1",
                "scenario_catalog_revision": "sha256:" + "a" * 64,
                "scenarios": [],
            }
        elif path == "/api/program/runs" and request.method == "GET":
            active_only = request.url.params.get("active_only") == "true"
            payload = {
                "schema_version": "manyforge.program_run_index.v1",
                "owner_id": "program-runner.cell-a",
                "owner_epoch": "program-runner.epoch.1",
                "active_only": active_only,
                "truncated": False,
                "runs": [],
            }
        elif path == "/api/program/runs" and request.method == "POST":
            payload = _program_run_status()
        elif path == "/api/program/runs/program-run.1":
            payload = _program_run_status()
        elif path == "/api/program/runs/program-run.1/stop":
            payload = _program_run_status(state="stopping")
        elif path == "/api/program/applied":
            if applied_program is None:
                return httpx.Response(
                    404,
                    json={
                        "detail": {
                            "code": "program_applied_selection_not_found",
                            "message": "no Program has been authoritatively Applied",
                        }
                    },
                    request=request,
                )
            payload = applied_program
        elif path == "/api/program/apply":
            assert isinstance(body, dict)
            if body["scenario_id"] == "pending-scenario" or (
                body["scenario_id"] == "revert-pending"
                and body["expected_applied_record_digest"] is not None
            ):
                return httpx.Response(
                    202,
                    json={
                        "schema_version": "manyforge.program_apply_result.v1",
                        "status": "pending_deployment",
                        "deployment_apply_required": True,
                        "preparation": {},
                        "manager_response": {"ok": True},
                        "applied": None,
                    },
                    request=request,
                )
            if (
                body["scenario_id"] in {"rejected-scenario", "revert-rejected"}
                and body["expected_applied_record_digest"] is not None
            ):
                return httpx.Response(
                    409,
                    json={
                        "detail": {
                            "code": "program_apply_compare_and_swap_failed",
                            "message": "foreign Apply won",
                        }
                    },
                    request=request,
                )
            if (
                body["scenario_id"] in {"malformed-scenario", "revert-malformed"}
                and body["expected_applied_record_digest"] is not None
            ):
                return httpx.Response(
                    200,
                    json={
                        "schema_version": "manyforge.program_apply_result.v1",
                        "status": "applied",
                        "applied": {"program_revision": "divergent"},
                    },
                    request=request,
                )
            previous_applied_program = applied_program
            program = body["program"]
            binding = {
                "api_version": "manyforge.program_binding.v1",
                "program_binding_id": "sha256:" + "0" * 64,
                "program_id": program["program_id"],
                "program_revision": program["program_revision"],
                "retained_bundle_id": "sha256:" + "4" * 64,
                "graph_revision": "sha256:" + "5" * 64,
                "node_environment": {
                    "catalog_id": "manyforge.default",
                    "content_version": "1.1.0",
                    "catalog_digest": "sha256:" + "1" * 64,
                    "environment_digest": "sha256:" + "2" * 64,
                },
                "component_role_bindings": [],
                "target_role_bindings": [],
                "contact_grasp_bindings": [],
                "provenance": {
                    "binding_compiler_contract_digest": "sha256:" + "3" * 64,
                    "binding_compiler_build_digest": "sha256:" + "4" * 64,
                    "role_assignment_digest": "sha256:" + "5" * 64,
                    "source_records": [],
                    "resolved_output_records": [],
                },
            }
            binding["program_binding_id"] = compute_content_identity(
                binding,
                own_field="program_binding_id",
                identity_domain="program-binding-id",
                api_version="manyforge.program_binding.v1",
            )
            committed_program = {
                "assistant_mode": "forbidden",
                "baseline_program_binding_id": "sha256:" + "3" * 64,
                "deployment_artifact_id": "cell_a",
                "deployment_revision": "sha256:" + "1" * 64,
                "graph_revision": "sha256:" + "5" * 64,
                "node_environment_digest": "sha256:" + "2" * 64,
                "preparation_digest": "sha256:" + "4" * 64,
                "program": program,
                "program_binding": binding,
                "program_binding_id": binding["program_binding_id"],
                "program_id": program["program_id"],
                "program_revision": program["program_revision"],
                "retained_bundle_id": "sha256:" + "4" * 64,
                "scenario_catalog_revision": body[
                    "scenario_catalog_revision"
                ],
                "scenario_id": body["scenario_id"],
                "schema_version": "manyforge.applied_program_selection.v1",
                "source": {
                    "record_digest": "sha256:" + "6" * 64,
                    "semantic_content_digest": "sha256:" + "7" * 64,
                    "source_generation": 1,
                    "source_owner_epoch": "program-owner.epoch.1",
                    "source_owner_id": "program-owner.cell-a",
                },
                "typed_parameter_overrides": {},
            }
            applied_program = (
                previous_applied_program
                if (
                    body["scenario_id"] == "revert-raced"
                    and body["expected_applied_record_digest"] is not None
                )
                else committed_program
            )
            payload = {
                "schema_version": "manyforge.program_apply_result.v1",
                "status": "applied",
                "deployment_apply_required": False,
                "preparation": {},
                "manager_response": None,
                "applied": committed_program,
            }
        elif path == "/api/assistant/modes":
            payload = {
                "schema_version": "manyforge.assistant_mode_index.v1",
                "modes": [{"mode": "composer-assistant"}],
            }
        elif path == "/api/assistant/modes/composer-assistant":
            payload = {
                "schema_version": "manyforge.assistant_mode_projection.v1",
                "mode": "composer-assistant",
            }
        elif path == "/api/manager/apply":
            return httpx.Response(
                503,
                json={
                    "error": {
                        "code": "kernel_owner_unavailable",
                        "message": "returns at I6",
                    },
                    "ok": False,
                },
                request=request,
            )
        elif path in {
            "/api/execution/stops",
            "/api/execution/full-stop",
            "/api/execution/stop-fault",
            "/api/execution/stop-fault/recover",
        } or path.startswith("/api/execution/operations/"):
            payload = {
                "api_version": "manyforge.execution_command_response.v1",
                "ok": True,
                "operation": {"operation_id": "operation-1"},
            }
        elif path in {
            "/api/execution/motion-previews",
            "/api/execution/capability-command-previews",
        }:
            payload = {
                "api_version": "manyforge.execution_command_response.v1",
                "command": (
                    "preview_motion"
                    if path.endswith("motion-previews")
                    else "preview_capability_command"
                ),
                "ok": True,
                "preview_receipt": {},
            }
        elif path == "/api/scene/transactions":
            if body["operation"]["scene_resource_id"] == "missing-part":
                return httpx.Response(
                    404,
                    json={
                        "api_version": "manyforge.manager_command_response.v1",
                        "committed_projection_revision": None,
                        "error": {
                            "code": "object_not_found",
                            "details": {"scene_resource_id": "missing-part"},
                            "message": "scene resource does not exist",
                        },
                        "ok": False,
                        "projection_status": "not_attempted",
                        "replay_status": "accepted_new",
                        "request_id": "request.scene.missing",
                        "request_owner_epoch": body["request_context"][
                            "request_owner_epoch"
                        ],
                        "transaction_sequence": None,
                        "update_status": None,
                    },
                    request=request,
                )
            payload = {
                "api_version": "manyforge.manager_command_response.v1",
                "committed_projection_revision": {
                    "retained_bundle_id": "sha256:" + "b" * 64,
                    "graph_revision": "sha256:" + "c" * 64,
                    "scene_revision": {
                        "scene_id": "sha256:" + "d" * 64,
                        "revision_id": 2,
                        "content_hash": "sha256:" + "b" * 64,
                    },
                },
                "error": None,
                "ok": True,
                "projection_status": "committed",
                "replay_status": "accepted_new",
                "request_id": "request.scene.1",
                "request_owner_epoch": body["request_context"][
                    "request_owner_epoch"
                ],
                "transaction_sequence": 1,
                "update_status": "geometry_changed",
            }
        elif path == "/api/scene":
            payload = {"schema_version": "manyforge.scene_projection.v1"}
        elif path == "/api/execution/alarms":
            payload = _alarms()
        elif path == "/api/execution/alarms/metrics":
            payload = {"schema_version": "manyforge.operator_alarm_metrics.v1"}
        elif path.endswith("/acknowledge") or path.endswith("/acknowledge-all"):
            payload = {"schema_version": "manyforge.headless_api_mutation_response.v1"}
        elif path == "/api/execution/history":
            payload = {"schema_version": "manyforge.historian_page.v1", "events": []}
        elif path == "/api/execution/history/health":
            payload = {
                "schema_version": "manyforge.historian_health.v1",
                "available": True,
            }
        elif path == "/api/execution/history/reconcile":
            payload = {
                "schema_version": "manyforge.historian_reconciliation.v1",
                "reconciled": True,
            }
        else:
            return httpx.Response(404, json={"detail": "unknown"}, request=request)
        return httpx.Response(200, json=payload, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        yield client, requests, node_environment
    finally:
        raw.close()


def _composer(client: HeadlessHttpClient) -> TestClient:
    return TestClient(create_app(ComposerState(headless_client=client)))


def _composer_with_program(client: HeadlessHttpClient) -> TestClient:
    state = ComposerState.from_paths(
        program_path=str(_PROGRAM_PATH),
        allowed_fs_roots=[str(_ROOT / "examples")],
        headless_client=client,
    )
    return TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(_ROOT / "examples")],
            ),
        )
    )


def _caller() -> dict[str, Any]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "alarm.epoch.1",
        "source_label": "composer_ui",
        "correlation_id": None,
    }


def test_composer_projects_headless_reads_without_local_runtime_state(
    headless_client,
) -> None:
    client, requests, node_environment = headless_client
    with _composer(client) as composer:
        health = composer.get("/api/health").json()
        assert health["mode"] == "headless_client"
        assert health["components"]["headless_projection"] == "degraded"
        assert health["headless"]["headless_projection_ready"] is False
        assert "ready" not in health["headless"]
        assert health["headless"]["robot_readiness"]["available"] is False
        assert composer.get("/api/runtime").json() == _runtime()
        assert composer.get("/api/manager/status").status_code == 200
        assert composer.get("/api/manager/graph").status_code == 200
        assert composer.get("/api/manager/diff").status_code == 200
        assert composer.get("/api/node-environment").json() == node_environment
        assert composer.get(
            "/api/assistant/modes/composer-assistant"
        ).json()["mode"] == "composer-assistant"
        assert composer.get("/api/assistant/modes").json()["modes"] == [
            {"mode": "composer-assistant"}
        ]
        assert composer.get("/api/source-model").json()["schema_version"] == (
            "manyforge.source_model_projection.v1"
        )
        catalog = composer.get("/api/catalog").json()
        assert catalog["environmentDigest"] == node_environment["environment_digest"]
        projected = composer.get("/api/nodes/environment").json()
        assert projected["active"] == {
            "retainedBundleId": "bundle.1",
            "graphRevision": "graph.1",
        }
        assert projected["availability"]["execution_available"] is False
        assert composer.get("/api/scene").status_code == 200
        assert (
            composer.get(
                "/api/execution/alarms?limit=3&ordering=attention_newest"
            ).json()
            == _alarms()
        )
        assert composer.get("/api/execution/history").status_code == 200

    assert all(item["method"] == "GET" for item in requests)
    alarm_request = next(
        item for item in requests if item["path"] == "/api/execution/alarms"
    )
    assert alarm_request["query"] == "limit=3&ordering=attention_newest"


def test_composer_source_model_uses_one_coherent_context_request(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    requests.clear()

    with _composer(client) as composer:
        response = composer.get("/api/source-model")

    assert response.status_code == 200
    assert response.json()["schema_version"] == (
        "manyforge.source_model_projection.v1"
    )
    assert [
        item["path"]
        for item in requests
        if item["path"]
        in {
            "/api/node-environment",
            "/api/source-model",
            "/api/source-model-context",
        }
    ] == ["/api/source-model-context"]


def test_composer_program_routes_are_strict_stateless_forwarders(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    digest_a = "sha256:" + "a" * 64
    start = {
        "request_id": "request.program.1",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
        "scenario_id": "cell-pick",
        "scenario_catalog_revision": digest_a,
        "expected_applied_record_digest": "sha256:" + "b" * 64,
    }
    stop = {
        "request_id": "request.program.2",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
        "program_run_id": "program-run.1",
        "expected_owner_epoch": "program-runner.epoch.1",
        "reason": "operator stop",
    }

    with _composer(client) as composer:
        assert composer.get("/api/scenarios").status_code == 200
        assert composer.get(
            "/api/program/runs?active_only=true&limit=1"
        ).json()["runs"] == []
        started = composer.post("/api/program/runs", json=start)
        read = composer.get("/api/program/runs/program-run.1")
        stopped = composer.post(
            "/api/program/runs/program-run.1/stop",
            json=stop,
        )
        mismatch = composer.post(
            "/api/program/runs/another-run/stop",
            json=stop,
        )

    assert started.status_code == 200
    assert read.json()["capture"]["program_run_id"] == "program-run.1"
    assert stopped.json()["state"] == "stopping"
    assert mismatch.status_code == 400
    forwarded = [
        request
        for request in requests
        if request["path"].startswith("/api/program/runs")
    ]
    assert [request["method"] for request in forwarded] == [
        "GET",
        "POST",
        "GET",
        "POST",
    ]


def test_composer_forwards_only_strict_attributed_alarm_intent(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    body = {
        **_caller(),
        "activation_id": "activation.1",
        "expected_record_revision": 2,
        "reason": None,
        "confirmed": True,
    }
    with _composer(client) as composer:
        accepted = composer.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json=body,
        )
        assert accepted.status_code == 200
        forged = composer.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json={**body, "trusted_principal_id": "forged"},
        )
        assert forged.status_code == 422

    forwarded = next(item for item in requests if item["method"] == "POST")
    assert forwarded["body"] == body
    assert "trusted_principal_id" not in forwarded["body"]


def test_composer_operation_controls_are_strict_stateless_forwarders(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    context = {
        "request_id": "request.control.1",
        "request_owner_epoch": "execution.epoch.1",
        "source_label": "composer_ui",
        "correlation_id": None,
    }
    control = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "reason": None,
    }
    stop = {
        "request_context": context,
        "operations": [
            {
                "operation_id": "operation-1",
                "expected_operation_revision": 3,
                "expected_stop_generation": 0,
            }
        ],
        "reason": "operator Stop",
    }
    scale = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "expected_runtime_scale_generation": 0,
        "scale": 0.5,
    }
    full_stop = {
        "request_context": {
            **context,
            "request_id": "request.full-stop.1",
        },
        "expected_stop_generation": 0,
    }
    recovery = {
        "request_context": {
            **context,
            "request_id": "request.stop-recovery.1",
        },
        "expected_latch_id": "stop-fault.1",
        "expected_latch_generation": 1,
        "expected_recovery_assessment_revision": 1,
        "expected_stop_certificate_id": "stop-certificate.1",
        "expected_stop_certificate_revision": 1,
        "expected_cell_id": "cell.1",
        "expected_retained_bundle_id": "sha256:" + "1" * 64,
        "expected_graph_revision": "sha256:" + "2" * 64,
        "configuration_expectations": [
            {
                "control_domain_id": "domain.1",
                "active_control_configuration_id": "configuration.1",
                "active_control_configuration_generation": 1,
            }
        ],
        "reason": None,
    }

    with _composer(client) as composer:
        for endpoint in ("pause", "resume", "reset", "acknowledge-completed"):
            response = composer.post(
                f"/api/execution/operations/operation-1/{endpoint}",
                json=control,
            )
            assert response.status_code == 200, response.text
        assert composer.post("/api/execution/stops", json=stop).status_code == 200
        assert (
            composer.post("/api/execution/full-stop", json=full_stop).status_code
            == 200
        )
        assert composer.get("/api/execution/stop-fault").status_code == 200
        assert (
            composer.post(
                "/api/execution/stop-fault/recover", json=recovery
            ).status_code
            == 200
        )
        assert (
            composer.post(
                "/api/execution/operations/operation-1/runtime-scale", json=scale
            ).status_code
            == 200
        )
        assert (
            composer.get(
                "/api/execution/operations/operation-1/runtime-scale"
            ).status_code
            == 200
        )
        mismatch = composer.post(
            "/api/execution/operations/operation-2/pause", json=control
        )

    assert mismatch.status_code == 400
    forwarded = [
        item
        for item in requests
        if item["path"] == "/api/execution/stops"
        or item["path"] == "/api/execution/full-stop"
        or item["path"].startswith("/api/execution/stop-fault")
        or item["path"].startswith("/api/execution/operations/")
    ]
    assert [item["path"].rsplit("/", 1)[-1] for item in forwarded] == [
        "pause",
        "resume",
        "reset",
        "acknowledge-completed",
        "stops",
        "full-stop",
        "stop-fault",
        "recover",
        "runtime-scale",
        "runtime-scale",
    ]
    assert forwarded[0]["body"] == control
    assert forwarded[4]["body"] == stop
    assert forwarded[5]["body"] == full_stop
    assert forwarded[6]["method"] == "GET"
    assert forwarded[7]["body"] == recovery
    assert forwarded[8]["body"] == scale
    assert forwarded[9]["method"] == "GET"


def test_composer_preserves_recognized_malformed_stop_semantics(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    mixed = {
        "request_context": {
            "request_id": "request.mixed-stop.1",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "operations": [
            {
                "operation_id": "operation-1",
                "expected_operation_revision": 3,
                "expected_stop_generation": 0,
            }
        ],
        "resources": [
            {
                "resource_id": "joint.1",
                "expected_graph_revision": "sha256:" + "1" * 64,
                "expected_operation_resource_generation": 4,
                "expected_stop_generation": 0,
            }
        ],
    }

    with _composer(client) as composer:
        malformed = composer.post(
            "/api/execution/stops",
            content=b'{"request_context":',
            headers={"content-type": "application/json"},
        )
        mixed_response = composer.post("/api/execution/stops", json=mixed)
        oversized = composer.post(
            "/api/execution/full-stop",
            content=b"x" * (1024 * 1024 + 1),
            headers={"content-type": "application/json"},
        )

    assert malformed.status_code == 200, malformed.text
    assert mixed_response.status_code == 200, mixed_response.text
    assert oversized.status_code == 200, oversized.text
    assert oversized.headers["connection"] == "close"
    forwarded = [
        item
        for item in requests
        if item["path"]
        in {"/api/execution/stops", "/api/execution/full-stop"}
    ]
    assert [item["path"] for item in forwarded] == [
        "/api/execution/stops",
        "/api/execution/stops",
        "/api/execution/full-stop",
    ]
    assert all(item["body"] == {} for item in forwarded)


def test_composer_preview_routes_are_strict_stateless_forwarders(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    body = {
        "request_context": {
            "request_id": "request.preview.1",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "program_id": "manyforge.direct_control.gripper",
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "b" * 64,
        "program_run_id": "direct-control-gripper-run.1",
        "expected_graph_revision": "sha256:" + "c" * 64,
        "expected_retained_bundle_id": "sha256:" + "d" * 64,
        "expected_scene_revision": {
            "scene_id": "sha256:" + "e" * 64,
            "revision_id": 1,
            "content_hash": "sha256:" + "f" * 64,
        },
        "participants": [
            {
                "component_role_id": "controlled_component",
                "target_role_id": "command_target",
                "required_capability_id": "tool.gripper.command",
                "action_span": None,
                "payload": {
                    "kind": "capability_command",
                    "command_schema_id": "gripper.command_position",
                    "command_schema_revision": "sha256:" + "1" * 64,
                    "command_value": {
                        "kind": "gripper_position",
                        "position": {"kind": "linear_m", "value_m": 0.02},
                        "max_effort": None,
                    },
                    "completion_timeout_s": 2.0,
                    "caller_options": [
                        {"option_id": "command_duration_s", "value_s": 0.5}
                    ],
                },
            }
        ],
        "coordination_policy": "independent",
    }

    with _composer(client) as composer:
        response = composer.post(
            "/api/execution/capability-command-previews", json=body
        )
        forged = composer.post(
            "/api/execution/capability-command-previews",
            json={**body, "trusted_principal_id": "forged"},
        )

    assert response.status_code == 200, response.text
    assert response.json()["command"] == "preview_capability_command"
    assert forged.status_code == 422
    forwarded = [
        item
        for item in requests
        if item["path"] == "/api/execution/capability-command-previews"
    ]
    assert forwarded == [
        {
            "method": "POST",
            "path": "/api/execution/capability-command-previews",
            "query": "",
            "body": body,
        }
    ]


def test_composer_scene_transaction_is_a_strict_stateless_forwarder(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    body = _scene_body()
    with _composer(client) as composer:
        response = composer.post("/api/scene/transactions", json=body)
        forged = composer.post(
            "/api/scene/transactions",
            json={**body, "attribution": {"transport": "http"}},
        )
    assert response.status_code == 200
    assert response.json()["projection_status"] == "committed"
    assert forged.status_code == 422
    forwarded = [item for item in requests if item["path"] == "/api/scene/transactions"]
    assert forwarded == [
        {
            "method": "POST",
            "path": "/api/scene/transactions",
            "query": "",
            "body": body,
        }
    ]


def test_program_scene_and_viewer_draft_edits_have_no_runtime_effect(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        workspace = composer.get("/api/program/workspace").json()
        table = workspace["draft"]["program"]["scene"][0]
        scene = composer.post(
            "/api/program/draft/mutations",
            json={
                "draftId": workspace["draft"]["draftId"],
                "expectedDraftRevision": workspace["draft"]["draftRevision"],
                "mutation": "scene_draft_update_object",
                "arguments": {"scene_resource": table},
            },
        )
        assert scene.status_code == 200, scene.text
        scene_workspace = scene.json()["workspace"]
        viewer = composer.post(
            "/api/program/draft/mutations",
            json={
                "draftId": scene_workspace["draft"]["draftId"],
                "expectedDraftRevision": scene_workspace["draft"][
                    "draftRevision"
                ],
                "mutation": "viewer_draft_replace",
                "arguments": {
                    "viewer": {
                        "frame_role_visibility": {"tcp": False},
                        "frame_gizmo_size_m": 0.1,
                        "object_axes_gizmo_size_m": 0.1,
                    }
                },
            },
        )
        assert viewer.status_code == 200, viewer.text

    assert not any(
        request["path"] == "/api/scene/transactions" for request in requests
    )


def test_composer_keeps_draft_and_save_independent_of_an_active_run(
    headless_client,
    tmp_path: Path,
) -> None:
    delegate, _requests, _node_environment = headless_client

    class ActiveRunnerClient:
        def __getattr__(self, name: str) -> Any:
            return getattr(delegate, name)

        def program_runs(
            self,
            *,
            active_only: bool,
            limit: int,
        ) -> dict[str, Any]:
            assert active_only is True
            assert limit == 1
            return {
                "schema_version": "manyforge.program_run_index.v1",
                "owner_id": "program-runner.cell-a",
                "owner_epoch": "program-runner.epoch.1",
                "active_only": True,
                "truncated": False,
                "runs": [_program_run_status()],
            }

    program_path = tmp_path / "active.program.yaml"
    program_path.write_bytes(_PROGRAM_PATH.read_bytes())
    state = ComposerState.from_paths(
        program_path=str(program_path),
        allowed_fs_roots=[str(tmp_path)],
        headless_client=ActiveRunnerClient(),  # type: ignore[arg-type]
    )
    snapshot = state.draft_session.snapshot()
    body = {
        "draftId": snapshot["draft_id"],
        "expectedDraftRevision": snapshot["draft_revision"],
    }
    client = TestClient(
        create_app(
            state,
            security=SecurityConfig(
                allowed_fs_roots=[str(tmp_path)],
            ),
        )
    )

    with client:
        saved = client.post("/api/program/save", json=body)
        mutated = client.post(
            "/api/program/draft/mutations",
            json={
                **body,
                "mutation": "program_draft_upsert_parameters",
                "arguments": {"parameters": []},
            },
        )

    assert saved.status_code == 200, saved.text
    assert mutated.status_code == 200, mutated.text
    assert state.draft_session.snapshot()["draft_revision"] == (
        snapshot["draft_revision"] + 1
    )


def test_composer_program_apply_advances_only_headless_applied_identity(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        response = composer.post(
            "/api/program/apply",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
                "scenarioId": "cell-pick",
                "scenarioCatalogRevision": "sha256:" + "7" * 64,
            },
        )

    assert response.status_code == 200, response.text
    workspace = response.json()["workspace"]
    assert workspace["draft"] == before["draft"]
    assert workspace["saved"] == before["saved"]
    assert workspace["applied"]["available"] is True
    assert workspace["applied"]["matchesDraft"] is True
    assert workspace["hasUnappliedChanges"] is False
    forwarded = [
        request for request in requests if request["path"] == "/api/program/apply"
    ]
    assert len(forwarded) == 1
    assert forwarded[0]["body"]["program"]["program_revision"] == (
        before["draft"]["programRevision"]
    )
    assert forwarded[0]["body"]["expected_applied_record_digest"] is None


def test_new_program_rotates_fenced_draft_without_disk_or_headless_mutation(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    disk_before = _PROGRAM_PATH.read_bytes()
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        request_offset = len(requests)
        created = composer.post(
            "/api/program/new-draft",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
                "confirmDiscard": False,
            },
        )
        replay = composer.post(
            "/api/program/new-draft",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
                "confirmDiscard": True,
            },
        )

    assert created.status_code == 200, created.text
    workspace = created.json()
    assert workspace["draft"]["draftId"] != before["draft"]["draftId"]
    assert workspace["draft"]["draftRevision"] == 1
    assert workspace["draft"]["program"]["name"] == "Untitled Program"
    assert workspace["draft"]["program"]["tree"]["node_id"] == "logic.check_bool"
    assert workspace["saved"]["available"] is False
    assert workspace["saved"]["path"] is None
    assert replay.status_code == 409
    assert replay.json()["detail"]["code"] == "composer_draft_id_stale"
    assert _PROGRAM_PATH.read_bytes() == disk_before
    assert all(
        request["method"] == "GET" for request in requests[request_offset:]
    )


def test_new_program_dirty_confirmation_precedes_owner_and_catalog_reads(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        dirty = composer.post(
            "/api/program/draft/mutations",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
                "mutation": "tree_draft_update_node_params",
                "arguments": {
                    "instance_id": "move_to_pick",
                    "expected_node_id": "motion.move_to_joint_target",
                    "parameters": {"planning_timeout_s": 3.0},
                },
            },
        ).json()["workspace"]
        request_offset = len(requests)
        rejected = composer.post(
            "/api/program/new-draft",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
                "confirmDiscard": False,
            },
        )

    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == (
        "composer_draft_discard_confirmation_required"
    )
    assert requests[request_offset:] == []


def test_normative_load_publishes_draft_and_saved_only_after_apply(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        loaded = composer.post(
            "/api/program/load",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
                "scenarioId": "cell-pick",
                "scenarioCatalogRevision": "sha256:" + "7" * 64,
                "path": str(_LOAD_PROGRAM_PATH),
                "confirmDiscard": True,
            },
        )

    assert loaded.status_code == 200, loaded.text
    workspace = loaded.json()["workspace"]
    assert workspace["draft"]["draftId"] != before["draft"]["draftId"]
    assert workspace["saved"]["path"] == str(_LOAD_PROGRAM_PATH.resolve())
    assert workspace["saved"]["matchesDraft"] is True
    assert workspace["applied"]["matchesDraft"] is True
    forwarded = [
        request for request in requests if request["path"] == "/api/program/apply"
    ]
    assert len(forwarded) == 1


def test_pending_normative_load_preserves_the_prior_workspace(
    headless_client,
) -> None:
    client, _requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        pending = composer.post(
            "/api/program/load",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
                "scenarioId": "pending-scenario",
                "scenarioCatalogRevision": "sha256:" + "7" * 64,
                "path": str(_LOAD_PROGRAM_PATH),
                "confirmDiscard": True,
            },
        )
        after = composer.get("/api/program/workspace").json()

    assert pending.status_code == 202
    assert pending.json()["status"] == "pending_deployment"
    assert pending.json()["workspace"] == before
    assert after == before


def test_program_discard_reads_owner_truth_without_runtime_mutation(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        initial = composer.get("/api/program/workspace").json()
        applied = composer.post(
            "/api/program/apply",
            json={
                "draftId": initial["draft"]["draftId"],
                "expectedDraftRevision": initial["draft"]["draftRevision"],
                "scenarioId": "cell-pick",
                "scenarioCatalogRevision": "sha256:" + "7" * 64,
            },
        ).json()["workspace"]
        dirty = composer.post(
            "/api/program/draft/mutations",
            json={
                "draftId": applied["draft"]["draftId"],
                "expectedDraftRevision": applied["draft"]["draftRevision"],
                "mutation": "tree_draft_update_node_params",
                "arguments": {
                    "instance_id": "move_to_pick",
                    "expected_node_id": "motion.move_to_joint_target",
                    "parameters": {"planning_timeout_s": 3.0},
                },
            },
        ).json()["workspace"]
        request_offset = len(requests)
        discarded = composer.post(
            "/api/program/discard",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
            },
        )
        no_op = composer.post(
            "/api/program/discard",
            json={
                "draftId": discarded.json()["draft"]["draftId"],
                "expectedDraftRevision": discarded.json()["draft"][
                    "draftRevision"
                ],
            },
        )

        assert discarded.status_code == 200, discarded.text
        assert discarded.json()["saved"] == {
            **dirty["saved"],
            "matchesDraft": True,
        }
    assert discarded.json()["draft"]["program"] == initial["draft"]["program"]
    assert no_op.json()["draft"]["draftRevision"] == discarded.json()["draft"][
        "draftRevision"
    ]
    action_requests = requests[request_offset:]
    assert all(request["method"] == "GET" for request in action_requests)
    assert {
        request["path"] for request in action_requests
    } == {"/api/program/applied", "/api/node-environment"}


def test_program_discard_without_applied_selection_is_typed_and_atomic(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        request_offset = len(requests)
        response = composer.post(
            "/api/program/discard",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
            },
        )
        after = composer.get("/api/program/workspace").json()

    assert response.status_code == 404
    assert response.json()["detail"]["code"] == (
        "program_applied_selection_not_found"
    )
    assert after["draft"] == before["draft"]
    assert after["saved"] == before["saved"]
    assert [
        request["method"]
        for request in requests[request_offset:]
        if request["path"] == "/api/program/applied"
    ] == ["GET", "GET"]


def test_program_revert_without_applied_selection_has_no_effect(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        request_offset = len(requests)
        response = composer.post(
            "/api/program/revert",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
            },
        )
        after = composer.get("/api/program/workspace").json()

    assert response.status_code == 404
    assert response.json()["detail"]["code"] == (
        "program_applied_selection_not_found"
    )
    assert after["draft"] == before["draft"]
    assert after["saved"] == before["saved"]
    assert not any(
        request["path"] == "/api/program/apply"
        for request in requests[request_offset:]
    )


@pytest.mark.parametrize(
    ("scenario_id", "expected_status", "expected_code"),
    [
        ("revert-pending", 202, None),
        ("revert-rejected", 409, "program_apply_compare_and_swap_failed"),
        ("revert-malformed", 502, "program_apply_response_invalid"),
        ("revert-raced", 409, "program_apply_selection_stale"),
    ],
)
def test_program_revert_failure_preserves_draft_saved_and_active_run(
    headless_client,
    scenario_id: str,
    expected_status: int,
    expected_code: str | None,
) -> None:
    client, requests, _node_environment = headless_client
    disk_before = _PROGRAM_PATH.read_bytes()
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        dirty = composer.post(
            "/api/program/draft/mutations",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
                "mutation": "tree_draft_update_node_params",
                "arguments": {
                    "instance_id": "move_to_pick",
                    "expected_node_id": "motion.move_to_joint_target",
                    "parameters": {"planning_timeout_s": 3.0},
                },
            },
        ).json()["workspace"]
        dirty = composer.post(
            "/api/program/apply",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
                "scenarioId": scenario_id,
                "scenarioCatalogRevision": "sha256:" + "7" * 64,
            },
        ).json()["workspace"]
        assert dirty["applied"]["programRevision"] != dirty["saved"][
            "programRevision"
        ]
        forbidden_offset = len(requests)
        caller_selected = composer.post(
            "/api/program/revert",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
                "scenarioId": "foreign",
                "scenarioCatalogRevision": "sha256:" + "f" * 64,
            },
        )
        assert caller_selected.status_code == 422
        assert requests[forbidden_offset:] == []
        capture_before = composer.get("/api/program/runs/program-run.1").json()[
            "capture"
        ]
        stale_offset = len(requests)
        stale = composer.post(
            "/api/program/revert",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"] - 1,
            },
        )
        assert stale.status_code == 409
        assert requests[stale_offset:] == []
        request_offset = len(requests)
        response = composer.post(
            "/api/program/revert",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
            },
        )
        after = composer.get("/api/program/workspace").json()
        capture_after = composer.get("/api/program/runs/program-run.1").json()[
            "capture"
        ]

    assert response.status_code == expected_status
    if expected_code is None:
        assert response.json()["status"] == "pending_deployment"
    else:
        assert response.json()["detail"]["code"] == expected_code
    assert after["draft"] == dirty["draft"]
    assert after["saved"] == dirty["saved"]
    assert _PROGRAM_PATH.read_bytes() == disk_before
    assert capture_after == capture_before
    apply_requests = [
        request
        for request in requests[request_offset:]
        if request["path"] == "/api/program/apply"
    ]
    assert len(apply_requests) == 1
    assert apply_requests[0]["body"]["program"] == before["draft"]["program"]
    assert apply_requests[0]["body"]["scenario_id"] == scenario_id
    assert apply_requests[0]["body"]["scenario_catalog_revision"] == (
        "sha256:" + "7" * 64
    )
    assert apply_requests[0]["body"]["expected_applied_record_digest"] == (
        "sha256:" + "6" * 64
    )


def test_program_revert_applies_exact_saved_content_and_preserves_run_capture(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        saved = composer.get("/api/program/workspace").json()
        dirty = composer.post(
            "/api/program/draft/mutations",
            json={
                "draftId": saved["draft"]["draftId"],
                "expectedDraftRevision": saved["draft"]["draftRevision"],
                "mutation": "tree_draft_update_node_params",
                "arguments": {
                    "instance_id": "move_to_pick",
                    "expected_node_id": "motion.move_to_joint_target",
                    "parameters": {"planning_timeout_s": 3.0},
                },
            },
        ).json()["workspace"]
        dirty = composer.post(
            "/api/program/apply",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
                "scenarioId": "cell-pick",
                "scenarioCatalogRevision": "sha256:" + "7" * 64,
            },
        ).json()["workspace"]
        assert dirty["applied"]["programRevision"] != dirty["saved"][
            "programRevision"
        ]
        capture_before = composer.get("/api/program/runs/program-run.1").json()[
            "capture"
        ]
        request_offset = len(requests)
        reverted = composer.post(
            "/api/program/revert",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
            },
        )
        no_op_offset = len(requests)
        no_op = composer.post(
            "/api/program/revert",
            json={
                "draftId": reverted.json()["workspace"]["draft"]["draftId"],
                "expectedDraftRevision": reverted.json()["workspace"]["draft"][
                    "draftRevision"
                ],
            },
        )
        capture_after = composer.get("/api/program/runs/program-run.1").json()[
            "capture"
        ]

    assert reverted.status_code == 200, reverted.text
    assert reverted.json()["status"] == "reverted"
    assert reverted.json()["workspace"]["draft"]["program"] == saved["draft"]["program"]
    assert reverted.json()["workspace"]["saved"] == saved["saved"]
    assert no_op.status_code == 200
    assert no_op.json()["status"] == "reverted"
    assert no_op.json()["headless"] is None
    assert no_op.json()["workspace"]["draft"]["draftRevision"] == (
        reverted.json()["workspace"]["draft"]["draftRevision"]
    )
    assert capture_after == capture_before
    apply_requests = [
        request
        for request in requests[request_offset:]
        if request["path"] == "/api/program/apply"
    ]
    assert len(apply_requests) == 1
    assert apply_requests[0]["body"]["program"] == saved["draft"]["program"]
    assert not any(
        request["path"] == "/api/program/apply"
        for request in requests[no_op_offset:]
    )


def test_composer_preserves_complete_scene_rejection(
    headless_client,
) -> None:
    client, _requests, _node_environment = headless_client
    body = _scene_body(
        request_id="request.scene.missing",
        scene_resource_id="missing-part",
    )
    with _composer(client) as composer:
        response = composer.post("/api/scene/transactions", json=body)
    assert response.status_code == 404
    assert response.json() == {
        "api_version": "manyforge.manager_command_response.v1",
        "committed_projection_revision": None,
        "error": {
            "code": "object_not_found",
            "details": {"scene_resource_id": "missing-part"},
            "message": "scene resource does not exist",
        },
        "ok": False,
        "projection_status": "not_attempted",
        "replay_status": "accepted_new",
        "request_id": "request.scene.missing",
        "request_owner_epoch": "scene-owner.epoch.7",
        "transaction_sequence": None,
        "update_status": None,
    }


def test_composer_preserves_headless_unavailability_code(
    headless_client,
) -> None:
    client, _requests, _node_environment = headless_client
    with _composer(client) as composer:
        response = composer.post(
            "/api/manager/apply",
            json={
                **_caller(),
                "deployment_artifact_id": "deployment.1",
                "deployment_revision": "sha256:" + "a" * 64,
                "expected_current": {
                    "graph_revision": "graph.1",
                    "retained_bundle_id": "bundle.1",
                },
                "reject_if_affected_busy": True,
            },
        )
    assert response.status_code == 503
    assert response.json()["detail"]["code"] == "kernel_owner_unavailable"


def test_composer_websocket_streams_owner_snapshots_only(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer(client) as composer:
        with composer.websocket_connect("/ws/state") as websocket:
            payload = websocket.receive_json()
            assert payload == {
                "schema_version": "manyforge.composer_headless_state.v1",
                "runtime": _runtime(),
                "alarms": _alarms(),
            }
    assert {item["path"] for item in requests} == {
        "/api/runtime",
        "/api/execution/alarms",
    }
    alarm_request = next(
        item for item in requests if item["path"] == "/api/execution/alarms"
    )
    assert alarm_request["query"] == "limit=32&ordering=attention_newest"


def test_composer_websocket_deduplicates_an_unchanged_owner_failure(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class RecordingWebSocket:
        def __init__(self) -> None:
            self.accepted = False
            self.messages: list[str] = []

        async def accept(self) -> None:
            self.accepted = True

        async def send_text(self, message: str) -> None:
            self.messages.append(message)

    class FailingClient:
        def __init__(self) -> None:
            self.polls = 0

        def runtime(self) -> dict[str, Any]:
            self.polls += 1
            if self.polls == 3:
                raise WebSocketDisconnect()
            raise HeadlessClientError(
                "headless_http_unavailable",
                "headless owner unavailable",
            )

        def alarms(self, *, limit: int, ordering: str) -> dict[str, Any]:
            del limit
            assert ordering == "attention_newest"
            return {}

    websocket = RecordingWebSocket()
    monkeypatch.setattr(headless_ws, "POLL_INTERVAL_SECONDS", 0)

    asyncio.run(serve_headless_state(websocket, FailingClient()))  # type: ignore[arg-type]

    assert websocket.accepted is True
    assert [json.loads(message) for message in websocket.messages] == [
        {
            "schema_version": "manyforge.composer_headless_state.v1",
            "runtime": None,
            "alarms": None,
            "error": {
                "code": "headless_http_unavailable",
                "message": "headless owner unavailable",
            },
        }
    ]


def test_composer_websocket_reports_an_oversized_projection_once(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class RecordingWebSocket:
        def __init__(self) -> None:
            self.messages: list[str] = []

        async def accept(self) -> None:
            return None

        async def send_text(self, message: str) -> None:
            self.messages.append(message)

    class SnapshotClient:
        def __init__(self) -> None:
            self.polls = 0

        def runtime(self) -> dict[str, Any]:
            self.polls += 1
            if self.polls == 2:
                raise WebSocketDisconnect()
            return _runtime()

        def alarms(self, *, limit: int, ordering: str) -> dict[str, Any]:
            assert limit == 32
            assert ordering == "attention_newest"
            return _alarms()

    original_encode = headless_ws._encode

    def fail_snapshot_only(value: dict[str, Any]) -> str:
        if value.get("runtime") is not None:
            raise ValueError("injected oversized snapshot")
        return original_encode(value)

    websocket = RecordingWebSocket()
    monkeypatch.setattr(headless_ws, "_encode", fail_snapshot_only)
    monkeypatch.setattr(headless_ws, "POLL_INTERVAL_SECONDS", 0)

    asyncio.run(serve_headless_state(websocket, SnapshotClient()))  # type: ignore[arg-type]

    assert [json.loads(message) for message in websocket.messages] == [
        {
            "schema_version": "manyforge.composer_headless_state.v1",
            "runtime": None,
            "alarms": None,
            "error": {
                "code": "headless_websocket_projection_oversized",
                "message": "headless WebSocket projection exceeds its byte bound",
            },
        }
    ]


def test_composer_rejects_a_mismatched_active_environment_without_local_fallback(
    headless_client,
) -> None:
    client, _requests, node_environment = headless_client
    node_environment["environment_digest"] = "sha256:" + "0" * 64
    with _composer(client) as composer:
        catalog = composer.get("/api/catalog")
        environment = composer.get("/api/nodes/environment")
    assert catalog.status_code == 503
    assert environment.status_code == 503
    assert catalog.json()["detail"]["code"] == ("node_environment_projection_invalid")


def test_composer_rejects_a_mismatched_local_adapter_disposition(
    headless_client,
) -> None:
    client, _requests, node_environment = headless_client
    row = next(
        item
        for item in node_environment["availability"]["implementations"]
        if item["node_id"] == "simulation.reset"
    )
    row["restoration_gates"] = ["I9", "N"]

    with _composer(client) as composer:
        response = composer.get("/api/catalog")

    assert response.status_code == 503
    assert response.json()["detail"]["code"] == ("node_environment_projection_invalid")


def test_composer_rejects_false_owner_health_for_an_owner_neutral_adapter(
    headless_client,
) -> None:
    client, _requests, node_environment = headless_client
    row = next(
        item
        for item in node_environment["availability"]["implementations"]
        if item["node_id"] == "sequence"
    )
    row["owner_health_available"] = False
    row["reason_codes"].append("program_runtime_owner_unavailable")

    with _composer(client) as composer:
        response = composer.get("/api/catalog")

    assert response.status_code == 503
    assert response.json()["detail"]["code"] == ("node_environment_projection_invalid")


def test_composer_does_not_fall_back_when_the_headless_environment_is_lost() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(
            503,
            json={
                "detail": {
                    "code": "manager_source_unavailable",
                    "message": "Manager publication is unavailable",
                }
            },
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        with _composer(client) as composer:
            response = composer.get("/api/catalog")
    finally:
        raw.close()
    assert response.status_code == 503
    assert response.json()["detail"]["code"] == "manager_source_unavailable"
_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM_PATH = _ROOT / "examples" / "pick_and_place_minimal.program.yaml"
_LOAD_PROGRAM_PATH = _ROOT / "examples" / "semantic_shape_pick.program.yaml"
