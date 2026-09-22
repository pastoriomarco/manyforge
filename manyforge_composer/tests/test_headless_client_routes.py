from __future__ import annotations

import asyncio
import copy
import json
import tempfile
import threading
from pathlib import Path
from typing import Any, Iterator
from urllib.parse import parse_qsl

import httpx
import pytest
from fastapi import FastAPI, WebSocketDisconnect
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
from manyforge_composer.backend.headless_ws import (
    HeadlessStateSource,
    serve_headless_state,
)
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_composer.backend.routes_viewer import create_viewer_router
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient

_BINDING_FIXTURE = (
    Path(__file__).resolve().parents[2]
    / "manyforge_deployment/resources/program_binding_lite6_native.fixture.json"
)


def _binding_for_program(program: dict[str, Any]) -> dict[str, Any]:
    binding = json.loads(_BINDING_FIXTURE.read_text(encoding="utf-8"))
    binding.update(
        {
            "program_binding_id": "sha256:" + "0" * 64,
            "program_id": program["program_id"],
            "program_revision": program["program_revision"],
            "retained_bundle_id": "sha256:" + "4" * 64,
            "graph_revision": "sha256:" + "5" * 64,
        }
    )
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    return binding


def _runtime() -> dict[str, Any]:
    def section(owner: str, generation: int) -> dict[str, Any]:
        return {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
            "payload": {},
            "source_generation": generation,
            "source_owner_epoch": f"{owner}.epoch.1",
            "source_owner_id": owner,
        }

    return {
        "schema_version": "manyforge.headless_runtime_snapshot.v1",
        "complete": True,
        "projection_owner_epoch": "projection.epoch.1",
        "projection_owner_id": "headless.owner",
        "projection_generation": 4,
        "reasons": [],
        "unavailable_sections": [],
        "sections": {
            "manager": section("manager", 1),
            "logical_scene": section("logical-scene", 2),
            "scene_runtime": section("scene-runtime", 3),
        },
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


def _component_overview() -> dict[str, Any]:
    runtime = _runtime()
    alarms = _alarms()
    sections = runtime["sections"]
    return {
        "schema_version": "manyforge.component_overview.v2",
        "overview_revision": "sha256:" + "a" * 64,
        "readiness": "ready",
        "summary_label": "Application surfaces ready",
        "profile_label": "native",
        "authority_label": "owner-issued scoped facts",
        "user_message": "Current owner facts are ready.",
        "capabilities": [
            "Observe / Render",
            "Plan / Preview",
            "Execute / Control",
        ],
        "components": [],
        "basis": {
            "runtime_projection_owner_epoch": runtime["projection_owner_epoch"],
            "runtime_projection_generation": runtime["projection_generation"],
            "manager_source_owner_epoch": sections["manager"]["source_owner_epoch"],
            "logical_scene_source_owner_epoch": sections["logical_scene"][
                "source_owner_epoch"
            ],
            "logical_scene_source_generation": sections["logical_scene"][
                "source_generation"
            ],
            "scene_runtime_source_owner_epoch": sections["scene_runtime"][
                "source_owner_epoch"
            ],
            "scene_runtime_source_generation": sections["scene_runtime"][
                "source_generation"
            ],
            "alarm_owner_epoch": alarms["owner_epoch"],
            "alarm_generation": alarms["generation"],
            "alarm_state_revision": alarms["state_revision"],
            "admission_generation": 7,
            "program_runner_owner_epoch": None,
            "program_cycle_generation": None,
            "applied_program_owner_epoch": None,
            "applied_program_generation": None,
            "execution_owner_epoch": None,
            "execution_publication_generation": None,
        },
    }


def _execution_observation() -> dict[str, Any]:
    return {
        "api_version": "manyforge.execution_command_response.v1",
        "command": "observation",
        "ok": True,
        "observation": {
            "schema_version": "manyforge.execution_observation.v2",
            "owner_epoch": "execution.epoch.1",
            "cell_id": "cell-a",
            "retained_bundle_id": "bundle.1",
            "graph_revision": "graph.1",
            "source_epoch": "joint-source.1",
            "source_sequence": 7,
            "targets": [
                {
                    "controllable_component_id": "robot-a",
                    "command_target_id": "arm",
                    "source_epoch": "arm-source.1",
                    "source_sequence": 7,
                    "source_joint_names": ["joint1"],
                    "compiled_joint_names": ["robot-a__joint1"],
                    "coordinate_kinds": ["angular"],
                    "positions_si": [0.25],
                }
            ],
        },
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
        "expected_admission_generation": 7,
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
        "schema_version": "manyforge.program_run_status.v2",
        "owner_id": "program-runner.cell-a",
        "owner_epoch": "program-runner.epoch.1",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v1",
            "program_run_id": "program-run.1",
            "applied_record_digest": digest_c,
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": digest_a,
            "program_id": "pick",
            "program_revision": digest_b,
            "program_binding_id": digest_c,
            "retained_bundle_id": digest_a,
            "graph_revision": digest_b,
            "node_environment_digest": digest_c,
            "node_policies": [],
            "node_policy_digest": "sha256:4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945",
            "owner_identities": [
                {
                    "owner_role": "program_runner",
                    "owner_id": "program-runner.cell-a",
                    "owner_epoch": "program-runner.epoch.1",
                }
            ],
        },
        "tree_status": "running",
        "node_status_revision": 1,
        "node_statuses": {"root": "running"},
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
        "schema_version": "manyforge.node_environment_projection.v2",
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "catalog_id": manifest.catalog_id,
        "content_version": manifest.catalog_content_version,
        "catalog_digest": manifest.catalog_digest,
        "environment_digest": manifest.environment_digest,
        "package_environment": {
            "catalog_id": manifest.catalog_id,
            "content_version": manifest.catalog_content_version,
            "catalog_digest": manifest.catalog_digest,
            "environment_digest": manifest.environment_digest,
        },
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
        "schema_version": "manyforge.source_model_projection.v2",
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
                b"manyforge.source_model_projection.v2",
                canonical_json_bytes(
                    {
                        key: value
                        for key, value in source_model.items()
                        if key != "source"
                    }
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
    scenario_selections: dict[str, dict[str, Any]] = {}
    scenario_cas_apply_counts: dict[str, int] = {}

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
                "components": {},
            }
        elif path == "/api/cell/authoring-admission":
            payload = {
                "authoring_allowed": True,
                "admission_generation": 7,
            }
        elif path == "/api/program/authoring-transactions/recovery-receipt":
            payload = {
                "recovery_receipt": {
                    "status": "unknown",
                    "kernel_owner_epoch": "kernel.1",
                    "held_claims": [],
                    "current_owner_epochs": [],
                }
            }
        elif path == "/api/program/authoring-transactions/reconcile-owner":
            payload = {
                "admission": {
                    "authoring_allowed": True,
                    "admission_generation": 7,
                }
            }
        elif path == "/api/program/authoring-transactions":
            intent = {
                "claim_kind": "authoring_mutation",
                "owner_role": "authoring_owner",
                "owner_epoch": body["request_owner_epoch"],
                "owner_transaction_id": body["owner_transaction_id"],
                "request_id": body["request_id"],
                "request_payload_digest": body["request_payload_digest"],
                "expected_admission_generation": body["expected_admission_generation"],
                "scope": {"resources": [], "configurations": []},
                "program_cycle_evidence": None,
                "intervention_evidence": None,
            }
            payload = {
                "claim": {
                    "intent": intent,
                    "claim_generation": 1,
                    "acquired_admission_generation": 7,
                    "accepted_at_ns": 1,
                }
            }
        elif path == "/api/program/authoring-transactions/settle":
            payload = {
                "admission": {
                    "authoring_allowed": True,
                    "admission_generation": 8,
                }
            }
        elif path == "/api/cycle/status":
            payload = {
                "schema_version": "manyforge.program_cycle_status.v1",
                "state": "idle",
            }
        elif path == "/api/control/state":
            payload = {
                "schema_version": "manyforge.execution_control_projection.v1",
                "semantic_owner_epoch": "program-runner.epoch.1",
                "execution_control_revision": 4,
            }
        elif path == "/api/control/commands":
            payload = {
                "receipt": {
                    "schema_version": "manyforge.execution_command_receipt.v1",
                    "correlation_id": body["command_envelope"]["correlation_id"],
                    "disposition": "received",
                },
                "result": {"accepted": True},
            }
        elif path == "/api/runtime":
            payload = _runtime()
        elif path == "/api/component-overview":
            payload = _component_overview()
        elif path == "/api/manager/status":
            payload = {
                "schema_version": "manyforge.manager_status_projection.v1",
                "source": {
                    "available": True,
                    "freshness": "fresh",
                    "known_gaps": [],
                    "source_owner_epoch": "manager.epoch.1",
                },
                "status": {
                    "manager_ready": True,
                    "active": {
                        "retained_bundle_id": (
                            applied_program["retained_bundle_id"]
                            if applied_program is not None
                            else "sha256:" + "4" * 64
                        ),
                        "graph_revision": (
                            applied_program["graph_revision"]
                            if applied_program is not None
                            else "sha256:" + "5" * 64
                        ),
                    },
                    "runtime": {"converged": True},
                },
            }
        elif path == "/api/manager/graph":
            payload = {"schema_version": "manyforge.manager_graph_projection.v1"}
        elif path == "/api/manager/diff":
            payload = {
                "schema_version": "manyforge.manager_diff_projection.v1",
                "source": {
                    "available": True,
                    "freshness": "fresh",
                    "known_gaps": [],
                },
                "converged": True,
            }
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
        elif path == "/api/program/runs/program-run.1/motion-stop":
            payload = _program_run_status(state="running")
        elif path == "/api/program/runs/program-run.1/motion-start":
            payload = _program_run_status(state="running")
        elif path == "/api/program/runs/program-run.1/full-stop":
            payload = _program_run_status(state="full_stopping")
        elif path == "/api/program/runs/program-run.1/resume-full-stop":
            payload = _program_run_status(state="running")
        elif path == "/api/program/runs/program-run.1/reset":
            payload = {
                "schema_version": "manyforge.program_cycle_reset_result.v1",
                "program_run_id": "program-run.1",
                "reset_cycle_generation": 4,
                "idle_cycle_generation": 5,
                "settlement_reference": "sha256:" + "d" * 64,
                "admission": {},
                "runtime_controls": None,
            }
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
        elif path.startswith("/api/scenarios/") and path.endswith("/program-selection"):
            scenario_id = path.split("/")[-2]
            selection = scenario_selections.get(scenario_id)
            if selection is None:
                return httpx.Response(404, json={"detail": "unknown"}, request=request)
            payload = copy.deepcopy(selection)
        elif path == "/api/program/apply":
            assert isinstance(body, dict)
            cas_attempt = scenario_cas_apply_counts.get(body["scenario_id"], 0)
            if (
                body["scenario_id"] == "pending-scenario"
                and body["expected_applied_record_digest"] is not None
            ) or (
                body["scenario_id"] == "revert-pending"
                and body["expected_applied_record_digest"] is not None
                and cas_attempt >= 1
            ):
                return httpx.Response(
                    409,
                    json={
                        "detail": {
                            "code": "program_apply_deployment_not_active",
                            "message": "select the deployment explicitly first",
                        }
                    },
                    request=request,
                )
            if (
                body["scenario_id"] in {"rejected-scenario", "revert-rejected"}
                and body["expected_applied_record_digest"] is not None
                and cas_attempt >= 1
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
                and cas_attempt >= 1
            ):
                malformed_program = body["program"]
                malformed_binding = _binding_for_program(malformed_program)
                malformed_binding["target_role_bindings"][0]["frame_role_bindings"][0][
                    "unknown"
                ] = True
                malformed_binding["program_binding_id"] = compute_content_identity(
                    malformed_binding,
                    own_field="program_binding_id",
                    identity_domain="program-binding-id",
                    api_version="manyforge.program_binding.v1",
                )
                return httpx.Response(
                    200,
                    json={
                        "schema_version": "manyforge.program_apply_result.v2",
                        "status": "applied",
                        "preparation": {},
                        "applied": {
                            "assistant_mode": "forbidden",
                            "baseline_program_binding_id": scenario_selections[
                                body["scenario_id"]
                            ]["program_binding_id"],
                            "deployment_artifact_id": "cell_a",
                            "deployment_revision": "sha256:" + "1" * 64,
                            "graph_revision": malformed_binding["graph_revision"],
                            "node_environment_digest": malformed_binding[
                                "node_environment"
                            ]["environment_digest"],
                            "preparation_digest": "sha256:" + "4" * 64,
                            "program": malformed_program,
                            "program_binding": malformed_binding,
                            "program_binding_id": malformed_binding[
                                "program_binding_id"
                            ],
                            "program_id": malformed_program["program_id"],
                            "program_revision": malformed_program["program_revision"],
                            "retained_bundle_id": malformed_binding[
                                "retained_bundle_id"
                            ],
                            "scenario_catalog_revision": body[
                                "scenario_catalog_revision"
                            ],
                            "scenario_id": body["scenario_id"],
                            "schema_version": (
                                "manyforge.applied_program_selection.v1"
                            ),
                            "source": {
                                "record_digest": "sha256:" + "6" * 64,
                                "semantic_content_digest": "sha256:" + "7" * 64,
                                "source_generation": 1,
                                "source_owner_epoch": "program-owner.epoch.1",
                                "source_owner_id": "program-owner.cell-a",
                            },
                            "typed_parameter_overrides": {},
                        },
                    },
                    request=request,
                )
            previous_applied_program = applied_program
            program = body["program"]
            binding = _binding_for_program(program)
            baseline = scenario_selections.get(body["scenario_id"])
            if baseline is None:
                baseline = {
                    "assistant_mode": "forbidden",
                    "assistant_projection": None,
                    "deployment_artifact_id": "cell_a",
                    "deployment_revision": "sha256:" + "1" * 64,
                    "graph_revision": "sha256:" + "5" * 64,
                    "node_environment_digest": binding["node_environment"][
                        "environment_digest"
                    ],
                    "owner_identities": [],
                    "program": copy.deepcopy(program),
                    "program_binding": copy.deepcopy(binding),
                    "program_binding_id": binding["program_binding_id"],
                    "program_id": program["program_id"],
                    "program_revision": program["program_revision"],
                    "retained_bundle_id": "sha256:" + "4" * 64,
                    "scenario_catalog_revision": body["scenario_catalog_revision"],
                    "scenario_id": body["scenario_id"],
                    "schema_version": "manyforge.scenario_program_selection.v2",
                    "typed_parameter_overrides": {},
                }
                scenario_selections[body["scenario_id"]] = baseline
            committed_program = {
                "assistant_mode": "forbidden",
                "baseline_program_binding_id": baseline["program_binding_id"],
                "deployment_artifact_id": "cell_a",
                "deployment_revision": "sha256:" + "1" * 64,
                "graph_revision": "sha256:" + "5" * 64,
                "node_environment_digest": binding["node_environment"][
                    "environment_digest"
                ],
                "preparation_digest": "sha256:" + "4" * 64,
                "program": program,
                "program_binding": binding,
                "program_binding_id": binding["program_binding_id"],
                "program_id": program["program_id"],
                "program_revision": program["program_revision"],
                "retained_bundle_id": "sha256:" + "4" * 64,
                "scenario_catalog_revision": body["scenario_catalog_revision"],
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
                    and cas_attempt >= 1
                )
                else committed_program
            )
            if body["expected_applied_record_digest"] is not None:
                scenario_cas_apply_counts[body["scenario_id"]] = cas_attempt + 1
            payload = {
                "schema_version": "manyforge.program_apply_result.v2",
                "status": "applied",
                "preparation": {},
                "applied": committed_program,
            }
        elif path == "/api/program/scene-preparations":
            assert applied_program is not None
            assert body["expected_applied_record_digest"] == (
                applied_program["source"]["record_digest"]
            )
            payload = {
                "schema_version": "manyforge.program_scene_preparation.v1",
                "status": "prepared",
                "applied_record_digest": applied_program["source"]["record_digest"],
                "program_revision": applied_program["program_revision"],
                "admission_generation": body["expected_admission_generation"],
                "prepared_scene": {
                    "schema_version": "manyforge.prepared_program_scene.v1",
                    "applied_record_digest": applied_program["source"]["record_digest"],
                    "program_revision": applied_program["program_revision"],
                },
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
            "/api/execution/runtime-scale-control",
        } or path.startswith("/api/execution/operations/"):
            payload = {
                "api_version": "manyforge.execution_command_response.v1",
                "ok": True,
                "operation": {"operation_id": "operation-1"},
            }
        elif path == "/api/execution/manual-handover":
            payload = {
                "api_version": "manyforge.execution_command_response.v1",
                "command": "read_manual_handover",
                "ok": True,
                "request_id": body["request_context"]["request_id"],
                "request_owner_epoch": body["request_context"]["request_owner_epoch"],
                "manual_handover": None,
                "availability": {
                    "available": False,
                    "cleanup_required": False,
                    "unavailable_reason": "Program motion is active",
                },
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
                "request_owner_epoch": body["request_context"]["request_owner_epoch"],
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
            payload = {
                "schema_version": "manyforge.historian_summary_page.v2",
                "events": [],
            }
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


class _ComposerTestClient(TestClient):
    """Add the backend-owned admission and durable request fence."""

    def __init__(
        self,
        state: ComposerState,
        *args,
        security: SecurityConfig | None = None,
        **kwargs,
    ) -> None:
        super().__init__(create_app(state, security=security), *args, **kwargs)
        self._composer_state = state
        self._authoring_sequence = 0

    def post(self, url, *, json=None, **kwargs):  # type: ignore[override]
        admission = self._composer_state.authoring_admission
        if url == "/api/scene/transactions" and isinstance(json, dict):
            operation = json.get("operation")
            operation_kind = (
                operation.get("kind") if isinstance(operation, dict) else None
            )
            if operation_kind in {
                "add_object",
                "upsert_object",
                "remove_object",
                "update_object_pose",
                "set_collision_policy",
            }:
                draft = self._composer_state.draft_session.snapshot()
                json = dict(json)
                json.setdefault("expected_draft_id", draft["draft_id"])
                json.setdefault("expected_draft_revision", draft["draft_revision"])
        if (
            admission is not None
            and isinstance(json, dict)
            and "draftId" in json
            and "expectedDraftRevision" in json
        ):
            self._authoring_sequence += 1
            workspace = self._composer_state.authoring_workspace.identity()
            json = {
                **json,
                "expectedAdmissionGeneration": 7,
                "requestOwnerEpoch": "test-browser.epoch.1",
                "requestId": f"test-browser.request.{self._authoring_sequence}",
            }
            if url not in {"/api/program/apply", "/api/program/revert"}:
                json.setdefault("expectedWorkspaceEpoch", workspace.workspace_epoch)
                json.setdefault(
                    "expectedWorkspaceRevision", workspace.workspace_revision
                )
        return super().post(url, json=json, **kwargs)


def _composer(client: HeadlessHttpClient) -> TestClient:
    state_root = tempfile.mkdtemp(prefix="manyforge-composer-journal-")
    return _ComposerTestClient(
        ComposerState(headless_client=client)
    )


def _composer_with_program(client: HeadlessHttpClient) -> TestClient:
    state = ComposerState.from_paths(
        program_path=str(_PROGRAM_PATH),
        allowed_fs_roots=[str(_ROOT / "examples")],
        headless_client=client,
    )
    return _ComposerTestClient(
        state,
        security=SecurityConfig(
            allowed_fs_roots=[str(_ROOT / "examples")],
        ),
    )


def test_public_control_websocket_rejects_an_unconfigured_origin(
    headless_client,
) -> None:
    client, _requests, _node_environment = headless_client
    with _composer(client) as composer:
        with pytest.raises(WebSocketDisconnect) as closed:
            with composer.websocket_connect(
                "/ws/control",
                headers={"origin": "https://untrusted.example"},
            ):
                pass
    assert closed.value.code == 1008


def test_public_control_rest_contract_is_a_strict_transparent_adapter(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    command = {
        "command_envelope": {
            "schema_version": "manyforge.execution_command.v1",
            "command_kind": "program_stop",
            "correlation_id": "request.external.stop.1",
            "source_label": "external_supervisor",
            "control_session_id": None,
            "client_sequence": None,
            "freshness": {
                "context_id": "freshness.1",
                "receiver_incarnation": "headless.receiver.1",
                "semantic_owner_epoch": "program-runner.epoch.1",
                "execution_control_revision": 4,
                "issued_server_sequence": "1",
                "valid_for_ns": 750_000_000,
            },
            "expected_semantic_owner_epoch": "program-runner.epoch.1",
            "expected_execution_control_revision": 4,
        },
        "payload": {},
    }
    with _composer(client) as composer:
        projection = composer.get("/api/control/state")
        accepted = composer.post("/api/control/commands", json=command)
        malformed = composer.post(
            "/api/control/commands",
            json={**command, "unexpected": True},
        )

    assert projection.json()["execution_control_revision"] == 4
    assert accepted.json()["receipt"]["correlation_id"] == ("request.external.stop.1")
    assert malformed.status_code == 422
    forwarded = [item for item in requests if item["path"].startswith("/api/control/")]
    assert [(item["method"], item["path"]) for item in forwarded] == [
        ("GET", "/api/control/state"),
        ("POST", "/api/control/commands"),
    ]


def _seed_applied_program(
    client: HeadlessHttpClient,
    program: dict[str, Any],
    *,
    scenario_id: str = "cell-pick",
) -> dict[str, Any]:
    """Establish owner truth before exercising Composer's Program-only Apply."""

    return client.program_apply(
        {
            "scenario_id": scenario_id,
            "scenario_catalog_revision": "sha256:" + "7" * 64,
            "program": program,
            "expected_applied_record_digest": None,
            "authoring_claim": {},
        }
    )


def _without_authoring_owner_calls(
    requests: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    return [
        request
        for request in requests
        if request["path"] != "/api/cell/authoring-admission"
        and not request["path"].startswith("/api/program/authoring-transactions")
    ]


def test_headless_http_client_rejects_rehashed_malformed_binding_ingress() -> None:
    program = {
        "program_id": "fixture",
        "program_revision": "sha256:" + "a" * 64,
    }
    binding = _binding_for_program(program)
    binding["target_role_bindings"][0]["frame_role_bindings"][0]["unknown"] = True
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    projection = {
        "program_binding": binding,
        "program_binding_id": binding["program_binding_id"],
        "program_id": binding["program_id"],
        "program_revision": binding["program_revision"],
        "retained_bundle_id": binding["retained_bundle_id"],
        "graph_revision": binding["graph_revision"],
        "node_environment_digest": binding["node_environment"]["environment_digest"],
    }

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json=projection, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    try:
        client = HeadlessHttpClient("http://unused.test", client=raw)
        with pytest.raises(HeadlessClientError) as raised:
            client.program_applied()
    finally:
        raw.close()
    assert raised.value.code == "program_applied_selection_invalid"


@pytest.mark.parametrize("record_kind", [[], {}])
def test_headless_http_client_maps_non_string_provenance_kind_to_stable_apply_error(
    record_kind,
) -> None:
    program = {
        "program_id": "fixture",
        "program_revision": "sha256:" + "a" * 64,
    }
    binding = _binding_for_program(program)
    binding["provenance"]["source_records"][0]["record_kind"] = record_kind
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    applied = {
        "assistant_mode": "forbidden",
        "baseline_program_binding_id": "sha256:" + "b" * 64,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": "sha256:" + "c" * 64,
        "graph_revision": binding["graph_revision"],
        "node_environment_digest": binding["node_environment"]["environment_digest"],
        "preparation_digest": "sha256:" + "d" * 64,
        "program": program,
        "program_binding": binding,
        "program_binding_id": binding["program_binding_id"],
        "program_id": program["program_id"],
        "program_revision": program["program_revision"],
        "retained_bundle_id": binding["retained_bundle_id"],
        "scenario_catalog_revision": "sha256:" + "e" * 64,
        "scenario_id": "cell-pick",
        "schema_version": "manyforge.applied_program_selection.v1",
        "source": {
            "record_digest": "sha256:" + "f" * 64,
            "semantic_content_digest": "sha256:" + "1" * 64,
            "source_generation": 1,
            "source_owner_epoch": "owner.epoch.1",
            "source_owner_id": "owner.cell-a",
        },
        "typed_parameter_overrides": {},
    }
    result = {
        "schema_version": "manyforge.program_apply_result.v2",
        "status": "applied",
        "preparation": {},
        "applied": applied,
    }

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json=result, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    try:
        client = HeadlessHttpClient("http://unused.test", client=raw)
        with pytest.raises(HeadlessClientError) as raised:
            client.program_apply({})
    finally:
        raw.close()
    assert raised.value.code == "program_apply_response_invalid"


def test_headless_http_client_rejects_rehashed_malformed_apply_binding() -> None:
    program = {
        "program_id": "fixture",
        "program_revision": "sha256:" + "a" * 64,
    }
    binding = _binding_for_program(program)
    binding["target_role_bindings"][0]["frame_role_bindings"][0]["unknown"] = True
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    applied = {
        "assistant_mode": "forbidden",
        "baseline_program_binding_id": "sha256:" + "b" * 64,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": "sha256:" + "c" * 64,
        "graph_revision": binding["graph_revision"],
        "node_environment_digest": binding["node_environment"]["environment_digest"],
        "preparation_digest": "sha256:" + "d" * 64,
        "program": program,
        "program_binding": binding,
        "program_binding_id": binding["program_binding_id"],
        "program_id": program["program_id"],
        "program_revision": program["program_revision"],
        "retained_bundle_id": binding["retained_bundle_id"],
        "scenario_catalog_revision": "sha256:" + "e" * 64,
        "scenario_id": "cell-pick",
        "schema_version": "manyforge.applied_program_selection.v1",
        "source": {
            "record_digest": "sha256:" + "f" * 64,
            "semantic_content_digest": "sha256:" + "1" * 64,
            "source_generation": 1,
            "source_owner_epoch": "owner.epoch.1",
            "source_owner_id": "owner.cell-a",
        },
        "typed_parameter_overrides": {},
    }
    result = {
        "schema_version": "manyforge.program_apply_result.v2",
        "status": "applied",
        "preparation": {},
        "applied": applied,
    }

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json=result, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    try:
        client = HeadlessHttpClient("http://unused.test", client=raw)
        with pytest.raises(HeadlessClientError) as raised:
            client.program_apply({})
    finally:
        raw.close()
    assert raised.value.code == "program_apply_response_invalid"


def _caller() -> dict[str, Any]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "alarm.epoch.1",
        "source_label": "composer_ui",
        "correlation_id": None,
    }


def test_component_overview_route_reads_one_fresh_headless_projection() -> None:
    class Client:
        calls = 0

        def component_overview_payload(self) -> bytes:
            self.calls += 1
            return json.dumps(
                {**_component_overview(), "summary_label": "cold read"}
            ).encode("utf-8")

    client = Client()
    app = FastAPI()
    app.include_router(
        create_headless_router(
            client,  # type: ignore[arg-type]
            ComposerState(),
        )
    )

    with TestClient(app) as composer:
        response = composer.get("/api/component-overview")
        assert response.status_code == 200
        assert response.json()["summary_label"] == "cold read"
        assert client.calls == 1


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
        assert "robot_readiness" not in health["headless"]
        assert composer.get("/api/runtime").json() == _runtime()
        assert composer.get("/api/component-overview").json() == (_component_overview())
        assert composer.get("/api/manager/status").status_code == 200
        assert composer.get("/api/manager/graph").status_code == 200
        assert composer.get("/api/manager/diff").status_code == 200
        assert composer.get("/api/node-environment").json() == node_environment
        assert (
            composer.get("/api/assistant/modes/composer-assistant").json()["mode"]
            == "composer-assistant"
        )
        assert composer.get("/api/assistant/modes").json()["modes"] == [
            {"mode": "composer-assistant"}
        ]
        assert composer.get("/api/source-model").json()["schema_version"] == (
            "manyforge.source_model_projection.v2"
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

    assert all(
        item["method"] == "GET" for item in _without_authoring_owner_calls(requests)
    )
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
    assert response.json()["schema_version"] == ("manyforge.source_model_projection.v2")
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
        "expected_admission_generation": 7,
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

    with _composer_with_program(client) as composer:
        workspace = composer.get("/api/program/workspace").json()
        start["draft_program_revision"] = workspace["draft"]["programRevision"]
        assert composer.get("/api/scenarios").status_code == 200
        assert (
            composer.get("/api/program/runs?active_only=true&limit=1").json()["runs"]
            == []
        )
        started = composer.post("/api/program/runs", json=start)
        read = composer.get("/api/program/runs/program-run.1")
        motion_stopped = composer.post(
            "/api/program/runs/program-run.1/motion-stop",
            json={
                **stop,
                "request_id": "request.program.motion-stop.1",
                "reason": "ordinary operator motion Stop",
            },
        )
        motion_started = composer.post(
            "/api/program/runs/program-run.1/motion-start",
            json={
                **stop,
                "request_id": "request.program.motion-start.1",
                "expected_run_revision": 1,
                "expected_selection_revision": 0,
                "reason": "initial operator motion Run",
            },
        )
        full_stopped = composer.post(
            "/api/program/runs/program-run.1/full-stop",
            json={
                **stop,
                "request_id": "request.program.full-stop.1",
                "reason": "ordinary operator Full Stop",
            },
        )
        resumed = composer.post(
            "/api/program/runs/program-run.1/resume-full-stop",
            json={
                **_caller(),
                "request_id": "request.program.resume-full-stop.1",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "expected_run_revision": 1,
                "reason": "operator Run after Full Stop",
            },
        )
        reset = composer.post(
            "/api/program/runs/program-run.1/reset",
            json={
                **_caller(),
                "request_id": "request.program.reset-full-stop.1",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "expected_run_revision": 1,
                "expected_cycle_generation": 4,
                "expected_claim_generation": 7,
                "reason": "operator Reset discarded Full Stop",
            },
        )
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
    assert motion_stopped.json()["state"] == "running"
    assert motion_started.json()["state"] == "running"
    assert full_stopped.json()["state"] == "full_stopping"
    assert resumed.json()["state"] == "running"
    assert reset.json()["idle_cycle_generation"] == 5
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
        "POST",
        "POST",
        "POST",
        "POST",
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

    forwarded = next(item for item in requests if item["path"].endswith("/acknowledge"))
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
    scale_control = {
        "request_context": context,
        "selector": {
            "program_binding_id": "binding-1",
            "component_role_id": "robot",
            "target_role_id": "arm",
        },
        "expected_control_generation": 0,
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
            composer.post("/api/execution/full-stop", json=full_stop).status_code == 200
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
                "/api/execution/runtime-scale-control", json=scale_control
            ).status_code
            == 200
        )
        assert (
            composer.get(
                "/api/execution/runtime-scale-control",
                params=scale_control["selector"],
            ).status_code
            == 200
        )
        assert (
            composer.get(
                "/api/execution/runtime-scale-control",
                params={
                    "program_binding_id": "binding-1",
                    "group_targets": (
                        '[{"component_role_id":"robot-a","target_role_id":"arm"},'
                        '{"component_role_id":"robot-b","target_role_id":"arm"}]'
                    ),
                },
            ).status_code
            == 200
        )
        assert (
            composer.get(
                "/api/execution/runtime-scale-control",
                params={
                    "program_binding_id": "binding-1",
                    "group_targets": (
                        '[{"component_role_id":"robot","component_role_id":"other",'
                        '"target_role_id":"arm"}]'
                    ),
                },
            ).status_code
            == 422
        )
        valid_group = (
            '[{"component_role_id":"robot-a","target_role_id":"arm"},'
            '{"component_role_id":"robot-b","target_role_id":"arm"}]'
        )
        assert (
            composer.get(
                "/api/execution/runtime-scale-control",
                params=[
                    ("program_binding_id", "binding-1"),
                    ("group_targets", "not-json"),
                    ("group_targets", valid_group),
                ],
            ).status_code
            == 422
        )
        for key in ("program_binding_id", "component_role_id", "target_role_id"):
            params = [
                ("program_binding_id", "binding-1"),
                ("component_role_id", "robot"),
                ("target_role_id", "arm"),
                (key, "first"),
                (key, "last"),
            ]
            assert (
                composer.get(
                    "/api/execution/runtime-scale-control", params=params
                ).status_code
                == 422
            )
        assert (
            composer.get(
                "/api/execution/runtime-scale-control",
                params={
                    "program_binding_id": "binding-1",
                    "group_targets": "[" * 2000 + "]" * 2000,
                },
            ).status_code
            == 422
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
        or item["path"] == "/api/execution/runtime-scale-control"
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
        "runtime-scale-control",
        "runtime-scale-control",
        "runtime-scale-control",
    ]
    assert forwarded[0]["body"] == control
    assert forwarded[4]["body"] == stop
    assert forwarded[5]["body"] == full_stop
    assert forwarded[6]["method"] == "GET"
    assert forwarded[7]["body"] == recovery
    assert forwarded[8]["body"] == scale_control
    assert forwarded[9]["method"] == "GET"
    assert forwarded[10]["method"] == "GET"
    assert json.loads(
        dict(parse_qsl(forwarded[10]["query"]))["group_targets"]
    ) == [
        {"component_role_id": "robot-a", "target_role_id": "arm"},
        {"component_role_id": "robot-b", "target_role_id": "arm"},
    ]


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
        if item["path"] in {"/api/execution/stops", "/api/execution/full-stop"}
    ]
    assert [item["path"] for item in forwarded] == [
        "/api/execution/stops",
        "/api/execution/stops",
        "/api/execution/full-stop",
    ]
    assert all(item["body"] == {} for item in forwarded)


@pytest.mark.parametrize(
    ("route", "command"),
    [
        ("capability-command-previews", "preview_capability_command"),
        ("manual-handover", "read_manual_handover"),
    ],
)
def test_composer_preview_and_handover_routes_are_strict_stateless_forwarders(
    headless_client,
    route,
    command,
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
        response = composer.post(f"/api/execution/{route}", json=body)
        forged = composer.post(
            f"/api/execution/{route}",
            json={**body, "trusted_principal_id": "forged"},
        )

    assert response.status_code == 200, response.text
    assert response.json()["command"] == command
    if command == "read_manual_handover":
        assert response.json() == {
            "api_version": "manyforge.execution_command_response.v1",
            "command": command,
            "ok": True,
            "request_id": body["request_context"]["request_id"],
            "request_owner_epoch": body["request_context"]["request_owner_epoch"],
            "manual_handover": None,
            "availability": {
                "available": False,
                "cleanup_required": False,
                "unavailable_reason": "Program motion is active",
            },
        }
    assert forged.status_code == 422
    forwarded = [item for item in requests if item["path"] == f"/api/execution/{route}"]
    assert forwarded == [
        {
            "method": "POST",
            "path": f"/api/execution/{route}",
            "query": "",
            "body": body,
        }
    ]


def test_composer_scene_transaction_fences_draft_and_forwards_only_owner_fields(
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
            "body": {**body, "program_node_evidence": None},
        }
    ]


def test_composer_runtime_scene_transaction_omits_authoring_fences(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    body = {
        "request_context": {
            "request_id": "request.scene.attach",
            "request_owner_epoch": "scene-owner.epoch.7",
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "expected_projection_revision": _scene_body()["expected_projection_revision"],
        "operation": {
            "kind": "attach_carried_object",
            "carried_object_attachment_id": "carried.part.1",
            "scene_resource_id": "part.1",
            "controllable_component_id": "robot.1",
            "command_target_id": "arm",
            "frame_role_id": "tcp",
            "touch_body_roles": [],
            "expected_component_attachment_revisions": [],
        },
    }
    with _composer(client) as composer:
        response = composer.post("/api/scene/transactions", json=body)
        over_fenced = composer.post(
            "/api/scene/transactions",
            json={**body, "expected_admission_generation": 7},
        )

    assert response.status_code == 200, response.text
    assert over_fenced.status_code == 422
    forwarded = [item for item in requests if item["path"] == "/api/scene/transactions"]
    assert forwarded == [
        {
            "method": "POST",
            "path": "/api/scene/transactions",
            "query": "",
            "body": {
                **body,
                "expected_admission_generation": None,
                "program_node_evidence": None,
            },
        }
    ]


def test_composer_rejects_stale_scene_draft_before_forwarding(headless_client) -> None:
    client, requests, _node_environment = headless_client
    body = {
        **_scene_body(),
        "expected_draft_id": "draft-stale",
        "expected_draft_revision": 99,
    }
    with _composer(client) as composer:
        response = composer.post("/api/scene/transactions", json=body)
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "scene_transaction_draft_stale"
    assert not any(item["path"] == "/api/scene/transactions" for item in requests)


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
                "expectedDraftRevision": scene_workspace["draft"]["draftRevision"],
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

    assert not any(request["path"] == "/api/scene/transactions" for request in requests)


def test_composer_rejects_draft_and_save_during_an_active_run(
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

        def authoring_admission(self) -> dict[str, Any]:
            return {"authoring_allowed": False, "admission_generation": 7}

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
    client = _ComposerTestClient(
        state,
        security=SecurityConfig(
            allowed_fs_roots=[str(tmp_path)],
        ),
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

    assert saved.status_code == 409, saved.text
    assert saved.json()["detail"]["code"] == "authoring_not_ready"
    assert mutated.status_code == 409, mutated.text
    assert mutated.json()["detail"]["code"] == "authoring_not_ready"
    assert (
        state.draft_session.snapshot()["draft_revision"] == snapshot["draft_revision"]
    )


def test_composer_program_apply_advances_only_headless_applied_identity(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        _seed_applied_program(client, before["draft"]["program"])
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
        response = composer.post(
            "/api/program/apply",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
            },
        )

    assert response.status_code == 200, response.text
    workspace = response.json()["workspace"]
    assert workspace["draft"] == dirty["draft"]
    assert workspace["saved"] == dirty["saved"]
    assert workspace["applied"]["available"] is True
    assert workspace["applied"]["matchesDraft"] is True
    assert workspace["hasUnappliedChanges"] is False
    forwarded = [
        request
        for request in requests[request_offset:]
        if request["path"] == "/api/program/apply"
    ]
    assert len(forwarded) == 1
    assert forwarded[0]["body"]["program"]["program_revision"] == (
        dirty["draft"]["programRevision"]
    )
    assert forwarded[0]["body"]["scenario_id"] == "cell-pick"
    assert forwarded[0]["body"]["expected_applied_record_digest"] == (
        "sha256:" + "6" * 64
    )
    prepared = [
        request
        for request in requests[request_offset:]
        if request["path"] == "/api/program/scene-preparations"
    ]
    assert len(prepared) == 1
    assert prepared[0]["body"]["expected_applied_record_digest"] == (
        "sha256:" + "6" * 64
    )
    assert requests.index(forwarded[0]) < requests.index(prepared[0])


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
    assert workspace["draft"]["program"] is None
    assert workspace["draft"]["candidateAvailable"] is False
    assert workspace["draft"]["authoring"]["name"] == "Untitled Program"
    assert workspace["draft"]["authoring"]["forest"] == []
    assert workspace["draft"]["diagnostics"][0]["code"] == (
        "authoring_forest_root_count"
    )
    assert workspace["saved"]["available"] is False
    assert workspace["saved"]["path"] is None
    assert replay.status_code == 409
    assert replay.json()["detail"]["code"] == "composer_draft_id_stale"
    assert _PROGRAM_PATH.read_bytes() == disk_before
    assert all(
        request["method"] == "GET"
        for request in _without_authoring_owner_calls(requests[request_offset:])
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


def test_program_only_load_route_is_removed(headless_client) -> None:
    client, _requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        before = composer.get("/api/program/workspace").json()
        rejected = composer.post(
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
        after = composer.get("/api/program/workspace").json()

    assert rejected.status_code == 410
    assert rejected.json()["detail"]["code"] == (
        "program_load_removed_use_scenario_load"
    )
    assert after == before


def test_program_discard_reads_owner_truth_without_runtime_mutation(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        initial = composer.get("/api/program/workspace").json()
        _seed_applied_program(client, initial["draft"]["program"])
        applied = composer.get("/api/program/workspace").json()
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
                "expectedDraftRevision": discarded.json()["draft"]["draftRevision"],
            },
        )

        assert discarded.status_code == 200, discarded.text
        assert discarded.json()["saved"] == {
            **dirty["saved"],
            "matchesDraft": True,
        }
    assert discarded.json()["draft"]["program"] == initial["draft"]["program"]
    assert (
        no_op.json()["draft"]["draftRevision"]
        == discarded.json()["draft"]["draftRevision"]
    )
    action_requests = _without_authoring_owner_calls(requests[request_offset:])
    assert all(request["method"] == "GET" for request in action_requests)
    assert {request["path"] for request in action_requests} == {
        "/api/program/applied",
        "/api/node-environment",
    }


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
    assert response.json()["detail"]["code"] == ("program_applied_selection_not_found")
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
    assert response.json()["detail"]["code"] == ("program_applied_selection_not_found")
    assert after["draft"] == before["draft"]
    assert after["saved"] == before["saved"]
    assert not any(
        request["path"] == "/api/program/apply" for request in requests[request_offset:]
    )


@pytest.mark.parametrize(
    ("scenario_id", "expected_status", "expected_code"),
    [
        (
            "revert-pending",
            409,
            "program_apply_deployment_not_active",
        ),
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
        _seed_applied_program(
            client,
            before["draft"]["program"],
            scenario_id=scenario_id,
        )
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
            },
        ).json()["workspace"]
        assert dirty["applied"]["programRevision"] != dirty["saved"]["programRevision"]
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


def test_program_revert_rejects_malformed_binding_without_effect_or_apply(
    headless_client,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        saved = composer.get("/api/program/workspace").json()
        _seed_applied_program(client, saved["draft"]["program"])
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
        applied_response = composer.post(
            "/api/program/apply",
            json={
                "draftId": dirty["draft"]["draftId"],
                "expectedDraftRevision": dirty["draft"]["draftRevision"],
            },
        ).json()
        before = applied_response["workspace"]
        malformed_projection = copy.deepcopy(applied_response["headless"]["applied"])
        malformed = malformed_projection["program_binding"]
        malformed["target_role_bindings"][0]["frame_role_bindings"][0]["unknown"] = True
        malformed["program_binding_id"] = compute_content_identity(
            malformed,
            own_field="program_binding_id",
            identity_domain="program-binding-id",
            api_version="manyforge.program_binding.v1",
        )
        malformed_projection["program_binding_id"] = malformed["program_binding_id"]
        monkeypatch.setattr(
            client,
            "program_applied",
            lambda: malformed_projection,
        )
        request_offset = len(requests)
        response = composer.post(
            "/api/program/revert",
            json={
                "draftId": before["draft"]["draftId"],
                "expectedDraftRevision": before["draft"]["draftRevision"],
            },
        )
        after = composer.get("/api/program/workspace").json()

    assert response.status_code == 502
    assert response.json()["detail"]["code"] == "program_applied_selection_invalid"
    assert after["draft"] == before["draft"]
    assert after["saved"] == before["saved"]
    assert not any(
        request["path"] == "/api/program/apply" for request in requests[request_offset:]
    )


def test_program_revert_applies_exact_saved_content_and_preserves_run_capture(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    with _composer_with_program(client) as composer:
        saved = composer.get("/api/program/workspace").json()
        _seed_applied_program(client, saved["draft"]["program"])
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
            },
        ).json()["workspace"]
        assert dirty["applied"]["programRevision"] != dirty["saved"]["programRevision"]
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
        request["path"] == "/api/program/apply" for request in requests[no_op_offset:]
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


def test_viewer_demand_lease_expires_without_waiting_for_wall_clock(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_composer.backend import execution_observation_stream

    class RecordingWebSocket:
        def __init__(self) -> None:
            self.accepted = False
            self.closed = False
            self.never_receives = asyncio.Event()

        async def accept(self) -> None:
            self.accepted = True

        async def receive_text(self) -> str:
            await self.never_receives.wait()
            raise AssertionError("the test socket must remain silent")

        async def send_text(self, _message: str) -> None:
            raise AssertionError("an uncredited viewer cannot receive a frame")

        async def close(self) -> None:
            self.closed = True

    class Subscription:
        def __init__(self) -> None:
            self.wake = asyncio.Event()

    class ObservationSource:
        def __init__(self) -> None:
            self.subscription: Subscription | None = None
            self.unsubscribed = False

        def subscribe(self) -> Subscription:
            self.subscription = Subscription()
            return self.subscription

        def unsubscribe(self, subscription: Subscription) -> None:
            assert subscription is self.subscription
            self.unsubscribed = True

        def grant_credit(self, _subscription: Subscription, _cursor) -> None:
            raise AssertionError("the expired client did not send credit")

        def take_payload(self, _subscription: Subscription) -> None:
            raise AssertionError("the expired client has no payload")

    async def exercise() -> None:
        source = ObservationSource()

        class ViewerState:
            execution_observation_source = source

        router = create_viewer_router(
            ViewerState(),  # type: ignore[arg-type]
            SecurityConfig(),
        )
        endpoint = next(
            route.endpoint
            for route in router.routes
            if getattr(route, "path", None) == "/ws/scene/viewer"
        )
        websocket = RecordingWebSocket()
        await endpoint(websocket)  # type: ignore[arg-type]
        assert websocket.accepted is True
        assert websocket.closed is True
        assert source.unsubscribed is True

    monkeypatch.setattr(
        execution_observation_stream,
        "OBSERVATION_DEMAND_LEASE_SECONDS",
        0.0,
    )
    asyncio.run(exercise())


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
