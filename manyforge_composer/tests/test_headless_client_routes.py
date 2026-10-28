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
        api_version="manyforge.program_binding.v2",
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
            "schema_version": "manyforge.execution_observation.v4",
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
                    "operation_capability_id": "execution.trajectory.execute",
                    "source_epoch": "arm-source.1",
                    "source_sequence": 7,
                    "source_joint_names": ["joint1"],
                    "compiled_joint_names": ["robot-a__joint1"],
                    "coordinate_kinds": ["angular"],
                    "positions_si": [0.25],
                }
            ],
            "measured_targets": [],
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
        "schema_version": "manyforge.program_run_status.v3",
        "owner_id": "program-runner.cell-a",
        "owner_epoch": "program-runner.epoch.1",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v2",
            "program_run_id": "program-run.1",
            "applied_record_digest": digest_c,
            "scenario_id": "cell-pick",
            "source_document_digest": digest_a,
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
        "details": None,
        "origin": None,
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
    def handler(request: httpx.Request) -> httpx.Response:
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
                "state": "idle_after_reset",
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
                        "retained_bundle_id": "sha256:" + "4" * 64,
                        "graph_revision": "sha256:" + "5" * 64,
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
            if body["operation"].get("scene_resource_id") == "missing-part":
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
        elif path == "/api/execution/history/captures":
            payload = {
                "schema_version": "manyforge.historian_capture_page.v1",
                "captures": [],
                "query": dict(request.url.params),
            }
        elif path == "/api/execution/program-runs/run-1/admitted-speeds":
            payload = {
                "schema_version": "manyforge.program_run_admitted_speeds.v1",
                "program_run_id": "run-1",
                "targets": [],
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
        elif path.startswith("/api/program/persistent-values"):
            payload = {"forwarded": path}
        elif path == "/api/execution/cell-recover":
            payload = {
                "schema_version": "manyforge.cell_recover_result.v1",
                "request_id": body["request_context"]["request_id"],
                "outcome": "done",
                "acknowledged_incidents": [],
                "latch_cleared": True,
                "recovered_components": [],
                "reason": None,
            }
        elif path == "/api/program/runs/program-run.7/node-correction-previews":
            payload = {"forwarded": path}
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
        api_version="manyforge.program_binding.v2",
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


def test_composer_passes_a_run_failure_record_through_unchanged() -> None:
    failed = {
        **_program_run_status(state="failed"),
        "tree_status": "failure",
        "node_statuses": {"root": "failure"},
        "reason_code": "owner_request_failed",
        "message": "frame role tool0 is not available",
        "details": {
            "reason": "frame_role_unresolved",
            "reference_type": "frame_role",
            "reference_id": "tool0",
            "target_role_id": "arm",
        },
        "origin": {
            "kind": "owner",
            "node_instance_id": "root",
            "owner_api": "scene.frames",
        },
    }

    def handler(request: httpx.Request) -> httpx.Response:
        assert request.url.path == "/api/program/runs/program-run.1"
        return httpx.Response(200, json=failed, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        state = ComposerState(headless_client=client)
        state.authoring_admission = None
        response = TestClient(create_app(state)).get("/api/program/runs/program-run.1")
    finally:
        raw.close()
    assert response.status_code == 200
    assert response.json() == failed


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
        captures = composer.get("/api/execution/history/captures?limit=4")
        assert captures.status_code == 200, captures.text
        assert captures.json()["query"] == {"limit": "4"}
        admitted = composer.get("/api/execution/program-runs/run-1/admitted-speeds")
        assert admitted.status_code == 200, admitted.text
        assert admitted.json()["program_run_id"] == "run-1"

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


def test_composer_program_routes_forward_when_draft_matches_applied(
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
        "source_document_digest": digest_a,
        "expected_applied_record_digest": "sha256:" + "b" * 64,
        # Q128: Run carries no admission generation; the server uses its current one.
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
        composer._composer_state.published_scenario = lambda: {  # type: ignore[method-assign]
            "ready": True,
            "published_scenario": {
                "program_revision": start["draft_program_revision"],
            },
        }
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
        for endpoint in ("reset", "acknowledge-completed"):
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
            "/api/execution/operations/operation-2/reset", json=control
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
    assert forwarded[2]["body"] == stop
    assert forwarded[3]["body"] == full_stop
    assert forwarded[4]["method"] == "GET"
    assert forwarded[5]["body"] == recovery
    assert forwarded[6]["body"] == scale_control
    assert forwarded[7]["method"] == "GET"
    assert forwarded[8]["method"] == "GET"
    assert json.loads(
        dict(parse_qsl(forwarded[8]["query"]))["group_targets"]
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
                "operation": {
                    **body["operation"],
                    "grasp_lost_bound": None,
                    "on_grasp_lost": None,
                },
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

        def program_cycle_status(self) -> dict[str, Any]:
            # A moving run: the held-run rule refuses the Draft edit.
            return {"state": "running", "program_run_id": None}

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


def test_program_revert_without_scenario_activation_input_has_no_effect(
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

    assert response.status_code == 409
    assert response.json()["detail"]["code"] == ("activation_input_incomplete")
    assert after["draft"] == before["draft"]
    assert after["saved"] == before["saved"]
    assert not any(
        request["path"] == "/api/manager/apply" for request in requests[request_offset:]
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
        state = ComposerState(headless_client=client)
        # This test covers the read adapter. Authoring-owner startup has its own
        # focused contract and must not obscure this unavailable read.
        state.authoring_admission = None
        response = TestClient(create_app(state)).get("/api/catalog")
    finally:
        raw.close()
    assert response.status_code == 503
    assert response.json()["detail"]["code"] == "manager_source_unavailable"


_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM_PATH = _ROOT / "examples" / "pick_and_place_minimal.program.yaml"
_LOAD_PROGRAM_PATH = _ROOT / "examples" / "semantic_shape_pick.program.yaml"


def test_composer_forwards_persistent_value_edits_as_strict_requests(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    base = "/api/program/persistent-values"
    caller = {
        "request_id": "request.1",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
        "project_path": "/projects/cell-pick.scenario",
    }
    edit = {
        **caller,
        "key": "layer",
        "expected_program_revision": "sha256:" + "a" * 64,
        "program_run_id": None,
        "expected_run_revision": None,
    }

    with _composer(client) as composer:
        assert composer.get(base).json() == {"forwarded": base}
        assert composer.post(base, json={**edit, "value": 3}).status_code == 200
        assert composer.post(f"{base}/reset", json=edit).status_code == 200
        assert composer.post(f"{base}/clear", json=caller).status_code == 200
        assert composer.post(base, json=edit).status_code == 422
        # The project folder must be absolute and normalized; never forwarded.
        relative = composer.post(
            f"{base}/clear", json={**caller, "project_path": "projects/cell-pick"}
        )

    assert relative.status_code == 422
    forwarded = [
        (item["method"], item["path"], item["body"])
        for item in requests
        if item["path"].startswith("/api/program/persistent-values")
    ]
    assert forwarded == [
        ("GET", base, None),
        ("POST", base, {**edit, "value": 3}),
        ("POST", f"{base}/reset", edit),
        ("POST", f"{base}/clear", caller),
    ]


def test_composer_forwards_the_node_correction_preview_as_a_strict_query(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    path = "/api/program/runs/program-run.7/node-correction-previews"
    body = {
        "schema_version": "manyforge.program_node_correction_preview_request.v1",
        "request_id": "node-preview.1",
        "program_run_id": "program-run.7",
        "expected_owner_epoch": "program-runner.epoch.1",
        "program_id": "lite6_program",
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "a" * 64,
        "node_instance_id": "move_to_pick",
        "node_attempt_id": "attempt:1",
        "proposed_inputs": [{"path": "goal.position_m[2]", "value": 0.25}],
    }

    with _composer(client) as composer:
        response = composer.post(path, json=body)
        mismatch = composer.post(
            "/api/program/runs/program-run.8/node-correction-previews", json=body
        )
        # The browser cannot name a binding, robot or capability.
        foreign = composer.post(
            path, json={**body, "target_role_id": "other_arm"}
        )

    assert response.status_code == 200, response.text
    assert response.json() == {"forwarded": path}
    assert mismatch.status_code == 400
    assert mismatch.json()["detail"]["code"] == "program_run_path_invalid"
    assert foreign.status_code == 422
    forwarded = [item for item in requests if item["path"].endswith("node-correction-previews")]
    assert forwarded == [{"method": "POST", "path": path, "query": "", "body": body}]



def test_composer_does_not_serve_removed_runtime_override_routes(headless_client) -> None:
    client, requests, _node_environment = headless_client
    removed = [
        "/api/program/runs/program-run.7/runtime-overrides",
        "/api/program/runs/program-run.7/runtime-overrides/activate",
        "/api/program/runs/program-run.7/runtime-overrides/clear",
        "/api/program/runs/program-run.7/runtime-overrides/promote",
        "/api/program/draft/runtime-override-promotions",
    ]
    with _composer(client) as composer:
        responses = [composer.post(path, json={}) for path in removed]
        served = composer.app.openapi()["paths"]
    # Only the generic unavailable-route answer remains; nothing is forwarded.
    assert [response.status_code for response in responses] == [503] * len(removed)
    assert {
        response.json()["detail"]["code"] for response in responses
    } == {"composable_cell_runtime_unavailable"}
    assert not any("runtime-override" in path for path in served)
    assert not any("runtime-override" in item["path"] for item in requests)


@pytest.mark.parametrize("result", [None, {"replay_status": "replayed_exact", "acceptance": None, "status": None}])
def test_lifecycle_request_result_proxy_preserves_exact_request_without_submit(result) -> None:
    body = {
        "request_context": _caller(), "controllable_component_id": "component.arm",
        "intent": "prepare", "expected_graph_revision": "graph.1",
        "expected_component_lifecycle_generation": 4,
        "expected_admission_generation": 7,
        "expected_control_domains": [{
            "kind": "controller",
            "control_domain_id": "domain.arm", "control_domain_revision": "domain.1",
            "controller_topology_id": "topology.arm", "topology_revision": "topology.1",
            "expected_active_control_configuration": {
                "presence": "absent", "active_control_configuration_id": None,
                "active_control_configuration_generation": None,
            },
            "expected_control_bindings": [{"control_binding_id": "binding.arm",
                "control_binding_revision": "binding.1", "control_route_id": "route.1"}],
        }],
        "reason": "operator-confirmed Composer request", "confirmed": True,
    }
    calls = []

    class Client:
        def component_lifecycle_request_result(self, payload):
            calls.append(payload)
            return {"result": result}

        def submit_component_lifecycle(self, payload):
            raise AssertionError("request-result read must never submit")

    app = FastAPI()
    app.include_router(create_headless_router(Client(), ComposerState()))
    with TestClient(app) as composer:
        response = composer.post("/api/cclc/lifecycle/requests/result", json=body)
    assert response.status_code == 200
    assert response.json() == {"result": result}
    assert calls == [body]


def _close_detach_body():
    body = _scene_body()
    body.pop("expected_admission_generation")
    body["operation"] = {"kind": "detach_carried_object_at_current_pose", "carried_object_attachment_id": "attachment.1",
        "expected_carried_object_attachment_revision": "sha256:" + "2" * 64, "expected_scene_resource_id": "part.1",
        "expected_controllable_component_id": "arm", "expected_command_target_id": "gripper"}
    return body


def test_close_detach_composite_fence_is_stripped_before_owner_forwarding(headless_client):
    client, requests, _ = headless_client
    with _composer(client) as composer:
        identity = composer._composer_state.authoring_workspace.identity()
        body = {**_close_detach_body(), "expected_authoring_workspace": identity.as_dict()}
        response = composer.post("/api/scene/transactions", json=body)
        assert response.status_code == 200, response.text
        owner = [request for request in requests if request["path"] == "/api/scene/transactions"]
        assert len(owner) == 1
        assert "expected_authoring_workspace" not in owner[0]["body"]
        assert "expected_draft_id" not in owner[0]["body"]
        assert owner[0]["body"].get("expected_admission_generation") is None


def test_close_composite_fence_is_refused_for_authored_scene_edits(headless_client):
    client, requests, _ = headless_client
    with _composer(client) as composer:
        identity = composer._composer_state.authoring_workspace.identity()
        response = composer.post("/api/scene/transactions", json={
            **_scene_body(), "expected_authoring_workspace": identity.as_dict(),
            "expected_draft_id": identity.program_draft_id,
            "expected_draft_revision": identity.program_draft_revision,
        })
        assert response.status_code == 422, response.text
        assert not [request for request in requests if request["path"] == "/api/scene/transactions"]


def test_close_detach_rechecks_concurrent_deployment_edit_under_shared_serializer(headless_client, monkeypatch):
    from manyforge_composer.backend.authoring_admission import AuthoringMutationEnvelope

    client, requests, _ = headless_client
    with _composer(client) as composer:
        state = composer._composer_state
        identity = state.authoring_workspace.identity()
        program_before = state.draft_session.snapshot()
        body = {**_close_detach_body(), "expected_authoring_workspace": identity.as_dict()}
        entered = threading.Event()
        proceed = threading.Event()
        original = state.authoring_admission.submit_workspace_fenced
        def delayed_submit(callback):
            entered.set()
            assert proceed.wait(5), "test never released Scene dispatch"
            return original(callback)
        monkeypatch.setattr(state.authoring_admission, "submit_workspace_fenced", delayed_submit)
        responses = []
        failures = []
        def submit_scene():
            try:
                responses.append(composer.post("/api/scene/transactions", json=body))
            except Exception as error:
                failures.append(error)
        thread = threading.Thread(target=submit_scene, daemon=True)
        thread.start()
        try:
            assert entered.wait(5), "Scene request did not reach serializer"
            replacement = state.deployment_draft_session.prepare_load_replacement({
                "schema_version": "manyforge.composer_deployment_draft_state.v2",
                "draft_id": identity.deployment_draft_id,
                "draft_revision": identity.deployment_draft_revision + 1,
                "document": {"api_version": "manyforge.deployment_intent.v2", "deployment_artifact_id": "changed",
                    "name": "Concurrent Deployment edit", "components": [], "attachments": [], "policies": {}},
                "saved_path": None, "saved_document": None,
            })
            # A real ordinary Deployment Draft edit takes this serializer, but
            # no Kernel claim: Program and admission generation stay unchanged.
            state.authoring_admission.execute_draft_edit(
                AuthoringMutationEnvelope(7, "test-browser.epoch.1", "deployment.concurrent"),
                operation_kind="deployment_edit", workspace=state.authoring_workspace,
                draft_id=identity.deployment_draft_id, expected_identity=identity,
                session="deployment", install=lambda: state.deployment_draft_session.install_prepared_replacement(replacement),
            )
        finally:
            proceed.set()
            thread.join(5)
        assert not thread.is_alive()
        assert failures == []
        assert len(responses) == 1
        assert responses[0].status_code == 409, responses[0].text
        assert responses[0].json()["detail"]["code"] == "composer_workspace_stale"
        assert state.draft_session.snapshot() == program_before
        assert not [request for request in requests if request["path"] == "/api/scene/transactions"]


@pytest.mark.parametrize("invalid", ["extra", "coerced_revision", "missing"])
def test_close_scene_composite_fence_is_strict_before_owner_effect(headless_client, invalid):
    client, requests, _ = headless_client
    with _composer(client) as composer:
        identity = composer._composer_state.authoring_workspace.identity().as_dict()
        if invalid == "extra": identity["unapproved"] = True
        elif invalid == "coerced_revision": identity["deploymentDraftRevision"] = "0"
        else: del identity["deploymentDraftId"]
        response = composer.post("/api/scene/transactions", json={**_scene_body(), "expected_authoring_workspace": identity})
        assert response.status_code == 422, response.text
        assert not [request for request in requests if request["path"] == "/api/scene/transactions"]


def test_close_runtime_detach_still_rejects_native_authoring_fences(headless_client):
    client, requests, _ = headless_client
    with _composer(client) as composer:
        body = _close_detach_body()
        body["expected_authoring_workspace"] = composer._composer_state.authoring_workspace.identity().as_dict()
        body["expected_admission_generation"] = 7
        response = composer.post("/api/scene/transactions", json=body)
        assert response.status_code == 422, response.text
        assert not [request for request in requests if request["path"] == "/api/scene/transactions"]


def test_composer_forwards_the_targeted_reset_acknowledgement_unchanged(
    headless_client,
) -> None:
    client, requests, _node_environment = headless_client
    control = {
        "request_context": {
            "request_id": "request.control.ack",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "reason": None,
        "acknowledged_uncertain_outputs": [
            {"operation_id": "operation-1", "operation_revision": 3}
        ],
    }
    with _composer(client) as composer:
        accepted = composer.post(
            "/api/execution/operations/operation-1/reset", json=control
        )
        refused = composer.post(
            "/api/execution/operations/operation-1/reset",
            json={
                **control,
                "acknowledged_uncertain_outputs": [
                    {"operation_id": "operation-9", "operation_revision": 1}
                ],
            },
        )
    assert accepted.status_code == 200
    assert refused.status_code == 422
    forwarded = [
        item
        for item in requests
        if item["path"] == "/api/execution/operations/operation-1/reset"
    ]
    assert [item["body"] for item in forwarded] == [control]


def test_global_io_rule_recovery_variant_is_proxied_unchanged(headless_client) -> None:
    client, requests, _node_environment = headless_client
    selector = {
        "context": {
            "owner_epoch": "kernel.epoch.1",
            "catalog_generation": "4",
            "active_publication_generation": 2,
            "bundle": "sha256:" + "1" * 64,
            "graph": "sha256:" + "2" * 64,
        },
        "rule_id": "door_open",
        "expected_rule_registration_id": "18446744073709551615",
    }
    recovery = {
        "kind": "global_io_rule",
        "request_context": {
            "request_id": "request.rule-recovery.1",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "composer_ui",
            "correlation_id": None,
        },
        "selector": selector,
        "reason": None,
    }
    with _composer(client) as composer:
        accepted = composer.post("/api/execution/stop-fault/recover", json=recovery)
        unknown = composer.post(
            "/api/execution/stop-fault/recover", json={**recovery, "kind": "other"}
        )
        numeric = composer.post(
            "/api/execution/stop-fault/recover",
            json={
                **recovery,
                "selector": {**selector, "expected_rule_registration_id": 7},
            },
        )
    assert accepted.status_code == 200, accepted.text
    assert unknown.status_code == 422
    assert numeric.status_code == 422
    forwarded = [
        item for item in requests if item["path"] == "/api/execution/stop-fault/recover"
    ]
    assert [item["body"] for item in forwarded] == [recovery]


def test_cell_recover_forwards_only_the_caller_identity(headless_client) -> None:
    client, requests, _node_environment = headless_client
    body = {
        "request_context": {
            "request_id": "request.cell-recover.1",
            "source_label": "composer_ui",
            "correlation_id": None,
        }
    }
    with _composer(client) as composer:
        accepted = composer.post("/api/execution/cell-recover", json=body)
        echoed = composer.post(
            "/api/execution/cell-recover",
            json={
                "request_context": {
                    **body["request_context"],
                    "request_owner_epoch": "execution.epoch.1",
                },
                "expected_latch_id": "stop-fault.1",
            },
        )
    assert accepted.status_code == 200, accepted.text
    assert accepted.json()["outcome"] == "done"
    assert echoed.status_code == 422
    forwarded = [item for item in requests if item["path"] == "/api/execution/cell-recover"]
    assert [item["body"] for item in forwarded] == [body]
