from __future__ import annotations

import copy
from functools import lru_cache
import hashlib
import json
import threading
from threading import Event
import time
from pathlib import Path
from typing import Any, Sequence

import pytest
import rfc8785
from fastapi.testclient import TestClient

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.executable_node_resolver import build_executable_node_resolver
from manyforge_behavior.node_availability import (
    AvailabilityFact,
    NodeExecutionReadiness,
    project_node_execution_availability,
)
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program

from manyforge_headless_api.app import (
    TrustedPrincipal,
    _error_status,
    _execution_observation_cursor,
    create_app,
)
from manyforge_headless_api.cell_admission import CellAdmissionError
from manyforge_headless_api.control_state import HeadlessControlStateSource
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    ManagerApplyRequest,
    ProgramCycleResetRequest,
    ProgramFullStopResetRequest,
    SceneResourcePoseQuery,
    SceneTransactionRequest,
    StopFaultRecoveryIntentRequest,
)
from manyforge_headless_api.native import HeadlessApiError, ViewerModelAsset
from manyforge_headless_api.program_runner_client import ProgramRunnerClientError
from manyforge_headless_api.service import HeadlessService, TrustedAttribution


_ROOT = Path(__file__).resolve().parents[2]
_BINDING_FIXTURE = (
    _ROOT / "manyforge_deployment/resources/program_binding_lite6_native.fixture.json"
)
_ADMISSION_GENERATION = 7
_AUTHORING_DIGEST = "sha256:" + "e" * 64


class FakeCellAdmission:
    """Strict replay-only admission owner for thin Headless fixtures."""

    def get(self) -> dict[str, Any]:
        return {
            "admission_generation": _ADMISSION_GENERATION,
            "authoring_allowed": True,
        }

    def acquire(self, intent: dict[str, Any]) -> dict[str, Any]:
        if set(intent) != {
            "claim_kind",
            "expected_admission_generation",
            "intervention_evidence",
            "owner_epoch",
            "owner_role",
            "owner_transaction_id",
            "program_cycle_evidence",
            "request_id",
            "request_payload_digest",
            "scope",
        }:
            raise CellAdmissionError(
                "authoring_claim_invalid",
                "fixture admission intent is not exact",
            )
        if (
            intent["claim_kind"] != "authoring_mutation"
            or intent["owner_role"] != "authoring_owner"
            or intent["expected_admission_generation"] != _ADMISSION_GENERATION
        ):
            raise CellAdmissionError(
                "stale_generation",
                "fixture admission generation is stale",
            )
        return {
            "intent": copy.deepcopy(intent),
            "claim_generation": 11,
            "acquired_admission_generation": _ADMISSION_GENERATION,
            "accepted_at_ns": 1,
        }


def _authoring_claim(
    *,
    request_id: str = "authoring.1",
    owner_transaction_id: str = "authoring-transaction.1",
) -> dict[str, Any]:
    return FakeCellAdmission().acquire(
        {
            "claim_kind": "authoring_mutation",
            "owner_role": "authoring_owner",
            "owner_epoch": "authoring-owner.epoch.1",
            "owner_transaction_id": owner_transaction_id,
            "request_id": request_id,
            "request_payload_digest": _AUTHORING_DIGEST,
            "expected_admission_generation": _ADMISSION_GENERATION,
            "scope": {"resources": [], "configurations": []},
            "program_cycle_evidence": None,
            "intervention_evidence": None,
        }
    )


@lru_cache(maxsize=1)
def _program_document() -> dict[str, Any]:
    environment = build_node_environment()
    return Program(
        _ROOT / "examples" / "pick_and_place_minimal.program.yaml",
        environment.catalog,
    ).to_dict()


def _program_binding(
    program: dict[str, Any],
    *,
    retained_bundle_id: str = "sha256:" + "8" * 64,
    graph_revision: str = "sha256:" + "9" * 64,
) -> dict[str, Any]:
    binding = json.loads(_BINDING_FIXTURE.read_text(encoding="utf-8"))
    binding["program_binding_id"] = "sha256:" + "0" * 64
    binding["node_environment"][
        "environment_digest"
    ] = build_node_environment().manifest.environment_digest
    binding["program_id"] = program["program_id"]
    binding["program_revision"] = program["program_revision"]
    binding["retained_bundle_id"] = retained_bundle_id
    binding["graph_revision"] = graph_revision
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v2",
    )
    return binding


def _applied_projection(
    program: dict[str, Any],
    *,
    scenario_id: str = "cell-pick",
    source_document_digest: str = "sha256:" + "6" * 64,
    record_digest: str = "sha256:" + "5" * 64,
    retained_bundle_id: str = "sha256:" + "8" * 64,
    graph_revision: str = "sha256:" + "9" * 64,
) -> dict[str, Any]:
    binding = _program_binding(
        program,
        retained_bundle_id=retained_bundle_id,
        graph_revision=graph_revision,
    )
    return {
        "assistant_mode": "forbidden",
        "assistant_projection": None,
        "baseline_program_binding_id": "sha256:" + "3" * 64,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": "sha256:" + "1" * 64,
        "graph_revision": binding["graph_revision"],
        "node_environment_digest": binding["node_environment"]["environment_digest"],
        "owner_identities": [{
            "owner_role": "headless_projection",
            "owner_id": "headless.program-apply",
            "owner_epoch": "headless.epoch.1",
        }],
        "preparation_digest": "sha256:" + "4" * 64,
        "program": copy.deepcopy(program),
        "program_binding": binding,
        "program_binding_id": binding["program_binding_id"],
        "program_id": program["program_id"],
        "program_revision": program["program_revision"],
        "retained_bundle_id": binding["retained_bundle_id"],
        "source_document_digest": source_document_digest,
        "scenario_id": scenario_id,
        "schema_version": "manyforge.applied_program_selection.v3",
        "source": {
            "record_digest": record_digest,
            "semantic_content_digest": "sha256:" + "7" * 64,
            "source_generation": 1,
            "source_owner_epoch": "program-owner.epoch.1",
            "source_owner_id": "program-owner.cell-a",
        },
    }


def _malform_binding_frame_with_valid_identity(value: dict[str, Any]) -> None:
    binding = value["program_binding"]
    binding["target_role_bindings"][0]["frame_role_bindings"][0]["unknown"] = True
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v2",
    )
    value["program_binding_id"] = binding["program_binding_id"]


def test_closed_core_invalid_request_maps_to_a_client_error() -> None:
    assert _error_status("invalid_request") == 400
    assert _error_status("full_stack_restart_required") == 409
    # Preparation refuses the input before any claim or effect.
    assert _error_status("activation_preparation_failed") == 409
    assert _error_status("manual_handover_required") == 409
    assert _error_status("deployment_candidate_cell_mismatch") == 409
    assert _error_status("manual_operation_scope_stale") == 409
    assert _error_status("program_required_component_not_active") == 409
    assert _error_status("route_selection_component_not_ready") == 409


@pytest.mark.parametrize(
    ("code", "expected_status"),
    [
        ("execution_owner_not_ready", 503),
        ("execution_command_response_invalid", 502),
        ("headless_api_internal_failure", 500),
        ("unexpected_owner_failure", 500),
    ],
)
@pytest.mark.parametrize(
    "projection", ["identity", "targets", "control_state", "diagnostics"]
)
@pytest.mark.parametrize("raise_error", [False, True])
def test_execution_projection_refusal_preserves_http_class_and_owner_details(
    code: str,
    expected_status: int,
    projection: str,
    raise_error: bool,
) -> None:
    execution = FakeExecution()
    message = "the managed execution owner has no active physical product"
    details = {"owner_epoch": "native-owner.empty"}

    envelope = {
        "api_version": "manyforge.execution_command_response.v1",
        "ok": False,
        "error": {"code": code, "message": message, "details": details},
    }

    def refuse() -> dict[str, Any]:
        # Projection reads return the owner envelope; typed callers can raise it.
        if raise_error:
            return ExecutionCommandClient.require_success(envelope)
        return envelope

    setattr(execution, projection, refuse)
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    with TestClient(app) as client:
        response = client.get(
            "/api/execution/" + projection.replace("_", "-"),
            headers={"X-Request-ID": "empty-runtime-check"},
        )
    assert response.status_code == expected_status
    if raise_error:
        assert response.json()["detail"] == {
            "code": code,
            "message": message,
            "details": details,
            "request_id": "empty-runtime-check",
        }
    else:
        assert response.json() == envelope
    assert response.headers["X-Request-ID"] == "empty-runtime-check"
    assert execution.requests == []


def test_full_native_applied_program_projection_is_accepted() -> None:
    core = FakeCore()
    projection = _applied_projection(_program_document())
    core.applied_program = copy.deepcopy(projection)
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/program/applied")

    assert response.status_code == 200
    assert response.json() == projection


def _applied_program_status_projection() -> dict[str, Any]:
    return {
        "deployment_artifact_id": "cell_a",
        "deployment_revision": "sha256:" + "1" * 64,
        "executable": True,
        "graph_revision": "sha256:" + "9" * 64,
        "message": None,
        "non_executable": None,
        "program": None,
        "program_binding_id": "sha256:" + "0" * 64,
        "program_id": "program_a",
        "program_revision": "sha256:" + "4" * 64,
        "record_digest": "sha256:" + "5" * 64,
        "retained_bundle_id": "sha256:" + "8" * 64,
        "scenario_id": "cell-pick",
        "schema_version": "manyforge.applied_program_status.v2",
        "source_document_digest": "sha256:" + "6" * 64,
    }


def test_applied_program_status_preserves_deployment_identity() -> None:
    core = FakeCore()
    status = _applied_program_status_projection()
    core.applied_program_status_projection = copy.deepcopy(status)
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/program/applied/status")

    assert response.status_code == 200
    assert response.json() == status


def test_applied_status_accepts_explicit_no_program_and_rejects_mixed_identity() -> None:
    core = FakeCore()
    status = _applied_program_status_projection()
    status.update(executable=False, program_id=None, program_revision=None,
                  program_binding_id=None, program=None, non_executable=None,
                  message="No Program is selected")
    core.applied_program_status_projection = status
    client = TestClient(create_app(service=_service(core, FakeManager())))
    assert client.get("/api/program/applied/status").json() == status
    status["program_binding_id"] = "sha256:" + "a" * 64
    response = client.get("/api/program/applied/status")
    assert response.status_code != 200
    assert response.json()["detail"]["code"] == "program_applied_selection_invalid"


@pytest.mark.parametrize(
    "malformation",
    [
        lambda value: value.pop("deployment_artifact_id"),
        lambda value: value.__setitem__("deployment_revision", "not-a-digest"),
        lambda value: value.__setitem__("unexpected", True),
    ],
)
def test_applied_program_status_rejects_invalid_deployment_identity(
    malformation,
) -> None:
    core = FakeCore()
    status = _applied_program_status_projection()
    malformation(status)
    core.applied_program_status_projection = status
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/program/applied/status")

    assert response.status_code != 200
    assert response.json()["detail"]["code"] == "program_applied_selection_invalid"


@pytest.mark.parametrize(
    "break_projection",
    [
        lambda value: value.__setitem__("unknown", True),
        lambda value: value.pop("program"),
        lambda value: value["program"].__setitem__("program_id", "mismatch"),
        lambda value: value["program"].__setitem__(
            "program_revision", "sha256:" + "f" * 64
        ),
        lambda value: value.pop("program_binding"),
        lambda value: value["program_binding"].__setitem__(
            "program_revision", "sha256:" + "f" * 64
        ),
        lambda value: value["program_binding"].__setitem__("unknown", True),
        lambda value: value["program_binding"]["node_environment"].__setitem__(
            "environment_digest", "sha256:" + "f" * 64
        ),
        lambda value: value["program_binding"].__setitem__(
            "component_role_bindings", [False]
        ),
        _malform_binding_frame_with_valid_identity,
        lambda value: value["source"].pop("semantic_content_digest"),
        lambda value: value["source"].__setitem__("record_digest", "invalid"),
    ],
)
def test_applied_program_projection_rejects_missing_or_divergent_closure(
    break_projection,
) -> None:
    core = FakeCore()
    projection = _applied_projection(_program_document())
    break_projection(projection)
    core.applied_program = projection
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/program/applied")

    assert response.status_code != 200
    assert response.json()["detail"]["code"] == "program_applied_selection_invalid"


@pytest.mark.parametrize(
    ("path", "invalid_value"),
    [
        (("baseline_program_binding_id",), "baseline.1"),
        (("deployment_revision",), "deployment.1"),
        (("graph_revision",), "graph.1"),
        (("node_environment_digest",), "environment.1"),
        (("preparation_digest",), "preparation.1"),
        (("program_revision",), "program.1"),
        (("program_binding_id",), "binding.1"),
        (("retained_bundle_id",), "bundle.1"),
        (("source_document_digest",), "catalog.1"),
        (("source", "record_digest"), "record.1"),
        (("source", "semantic_content_digest"), "semantic.1"),
    ],
)
def test_applied_program_projection_rejects_malformed_exact_identity(
    path: tuple[str, ...],
    invalid_value: object,
) -> None:
    core = FakeCore()
    projection = _applied_projection(_program_document())
    target = projection
    for field in path[:-1]:
        target = target[field]
    target[path[-1]] = invalid_value
    core.applied_program = projection
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/program/applied")

    assert response.status_code != 200
    assert response.json()["detail"]["code"] == "program_applied_selection_invalid"


@pytest.mark.parametrize("source_generation", [True, False, 0])
def test_applied_program_projection_rejects_invalid_source_generation(
    source_generation: object,
) -> None:
    core = FakeCore()
    projection = _applied_projection(_program_document())
    projection["source"]["source_generation"] = source_generation
    core.applied_program = projection
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/program/applied")

    assert response.status_code != 200
    assert response.json()["detail"]["code"] == "program_applied_selection_invalid"


def _manager_payload() -> dict[str, Any]:
    offer_digest = "sha256:" + "c" * 64
    return {
        "api_version": "manyforge.manager_source.v1",
        "active": {
            "cell_graph": {"cell_id": "cell-a", "components": []},
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
            "publication": {},
        },
        "apply": {
            "mutation_available": False,
            "records": [],
            "unavailable_reason_code": "kernel_owner_unavailable",
        },
        "cell_id": "cell-a",
        "manager_ready": True,
        "runtime": {
            "converged": True,
            "desired_units": [
                {
                    "runtime_unit_id": "runtime.fake",
                    "selected_runtime_contract_offer_digest": offer_digest,
                }
            ],
            "observed_units": [
                {
                    "runtime_unit_id": "runtime.fake",
                    "observed_runtime_contract_offer_digest": offer_digest,
                }
            ],
        },
    }


def _section(owner: str, payload: dict[str, Any]) -> dict[str, Any]:
    return {
        "available": True,
        "freshness": "fresh",
        "known_gaps": [],
        "payload": payload,
        "projection_generation": 4,
        "receiver_age_ns": 10,
        "source_content_digest": "sha256:" + "a" * 64,
        "source_generation": 2,
        "source_observed_at_ns": 1,
        "source_owner_epoch": "epoch.1",
        "source_owner_id": owner,
        "source_owner_kind": "test",
    }


class FakeCore:
    def __init__(self) -> None:
        self.acknowledgements: list[dict[str, Any]] = []
        self.historian_reconciliations: list[dict[str, Any]] = []
        self.manager_payload = _manager_payload()
        self.program_preparations: list[dict[str, Any]] = []
        self.program_commits: list[tuple[dict[str, Any], str | None]] = []
        self.program_replays: dict[
            tuple[str, str], tuple[tuple[object, ...], dict[str, Any]]
        ] = {}
        self.applied_program: dict[str, Any] | None = None
        self.applied_program_status_projection: dict[str, Any] | None = None
        self.program_target_bundle = "bundle.1"
        self.program_target_graph = "graph.1"
        self.model_attempts: list[dict[str, Any]] = []
        self.source_model_context_override: dict[str, Any] | None = None
        self.node_environment_digest = (
            build_node_environment().manifest.environment_digest
        )

    def runtime_snapshot(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.headless_runtime_snapshot.v1",
            "complete": True,
            "projection_generation": 4,
            "projection_owner_epoch": "projection.epoch.1",
            "reasons": [],
            "sections": {
                "manager": _section("component-manager.cell-a", self.manager_payload),
                "logical_scene": _section(
                    "logical-scene.cell-a",
                    {"api_version": "manyforge.logical_scene_source.v1"},
                ),
                "scene_runtime": _section(
                    "scene-runtime.cell-a",
                    {"api_version": "manyforge.scene_runtime_source.v1"},
                ),
            },
        }

    def node_environment_projection(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.node_environment_projection.v2",
            "retained_bundle_id": "bundle.1",
            "graph_revision": "graph.1",
            "environment_digest": self.node_environment_digest,
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "projection_generation": 1,
                "projection_owner_epoch": "projection.epoch.1",
                "receiver_age_ns": 10,
                "source_content_digest": "sha256:" + "b" * 64,
                "source_generation": 1,
                "source_owner_epoch": "projection.epoch.1",
                "source_owner_id": "headless-api.node-environment",
            },
        }

    def source_model_projection(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.source_model_projection.v2",
            "retained_bundle_id": "bundle.1",
            "graph_revision": "graph.1",
            "occurrences": [],
            "targets": [],
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "source_owner_id": "headless-api.source-model",
            },
        }

    def source_model_context(self) -> dict[str, Any]:
        if self.source_model_context_override is not None:
            return copy.deepcopy(self.source_model_context_override)
        return {
            "schema_version": "manyforge.source_model_context.v1",
            "node_environment": self.node_environment_projection(),
            "source_model": self.source_model_projection(),
        }

    def assistant_mode_context(self, mode_id: str) -> dict[str, Any]:
        return {
            "assistant_mode": self.assistant_mode_projection(mode_id),
            "node_environment": self.node_environment_projection(),
            "schema_version": "manyforge.assistant_mode_context.v1",
            "source_model": self.source_model_projection(),
        }

        return copy.deepcopy(self.applied_program)

    def applied_program_selection(self) -> dict[str, Any]:
        if self.applied_program is None:
            raise HeadlessApiError(
                "program_applied_selection_not_found",
                "no Program has been authoritatively Applied",
            )
        return copy.deepcopy(self.applied_program)

    def committed_scenario_activation(self) -> dict[str, Any]:
        activation = getattr(self, "committed_activation", None)
        if activation is None:
            raise HeadlessApiError(
                "program_applied_selection_not_found",
                "no Program has been authoritatively Applied",
            )
        return copy.deepcopy(activation)

    def applied_program_status(self) -> dict[str, Any]:
        if self.applied_program_status_projection is None:
            raise HeadlessApiError(
                "program_applied_selection_not_found",
                "no Program has been authoritatively Applied",
            )
        return copy.deepcopy(self.applied_program_status_projection)

    def commit_program_model_attempt(
        self,
        record: dict[str, Any],
    ) -> dict[str, Any]:
        self.model_attempts.append(copy.deepcopy(record))
        return {
            "schema_version": "manyforge.program_model_attempt_commit.v1",
            "model_attempt_id": record["model_attempt_id"],
            "event_id": "event.1",
            "status": "stored",
        }

    def viewer_model_urdf(self) -> ViewerModelAsset:
        return ViewerModelAsset(
            asset_kind="urdf",
            cell_id="cell-a",
            graph_revision="graph.1",
            retained_bundle_id="sha256:" + "1" * 64,
            product_id="cell_viewer_model",
            product_contract_id="manyforge.viewer_model",
            product_contract_revision="sha256:" + "2" * 64,
            viewer_model_revision="sha256:" + "3" * 64,
            logical_uri=None,
            media_type="application/xml",
            byte_size=23,
            content_digest="sha256:" + "4" * 64,
            bytes=b"<robot name='fixture'/>",
        )

    def viewer_model_resource(
        self,
        expected_retained_bundle_id: str,
        logical_uri: str,
    ) -> ViewerModelAsset:
        return ViewerModelAsset(
            asset_kind="resource",
            cell_id="cell-a",
            graph_revision="graph.1",
            retained_bundle_id=expected_retained_bundle_id,
            product_id="cell_viewer_model",
            product_contract_id="manyforge.viewer_model",
            product_contract_revision="sha256:" + "2" * 64,
            viewer_model_revision="sha256:" + "3" * 64,
            logical_uri=logical_uri,
            media_type="model/stl",
            byte_size=5,
            content_digest="sha256:" + "5" * 64,
            bytes=b"solid",
        )

    def assistant_mode_projection(self, mode_id: str) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.assistant_mode_projection.v1",
            "mode": mode_id,
            "retained_bundle_id": "bundle.1",
            "graph_revision": "graph.1",
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "source_owner_id": "headless-api.assistant-mode",
            },
        }

    def assistant_mode_index(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.assistant_mode_index.v1",
            "retained_bundle_id": "bundle.1",
            "graph_revision": "graph.1",
            "modes": [
                {
                    "mode": "composer-assistant",
                    "description": "Draft authoring",
                    "grant_policy": {},
                }
            ],
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "source_owner_id": "headless-api.assistant-mode-index",
            },
        }

    def alarm_snapshot(
        self,
        limit: int | None = None,
        states: Sequence[str] = (),
        ordering: str = "default",
    ) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.operator_alarm_snapshot.v2",
            "owner_epoch": "alarm.epoch.1",
            "generation": 3,
            "reconciled": True,
            "health": {"state": "available", "details": {}},
            "alarms": [],
            "requested_limit": limit,
            "requested_states": list(states),
            "requested_ordering": ordering,
        }

    def alarm_metrics(self) -> dict[str, Any]:
        return {"schema_version": "manyforge.operator_alarm_metrics.v1"}

    def acknowledge_alarm(self, **kwargs: Any) -> dict[str, Any]:
        self.acknowledgements.append(kwargs)
        return {"schema_version": "manyforge.headless_api_mutation_response.v1"}

    def acknowledge_all(self, **kwargs: Any) -> dict[str, Any]:
        self.acknowledgements.append(kwargs)
        return {"schema_version": "manyforge.headless_api_mutation_response.v1"}

    def history(self, **kwargs: Any) -> dict[str, Any]:
        del kwargs
        return {
            "schema_version": "manyforge.historian_summary_page.v2",
            "events": [],
            "next_before_sequence": None,
            "truncated": False,
        }

    def history_detail(self, sequence: int) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.historian_detail.v2",
            "sequence": sequence,
            "event": {
                "schema_version": "manyforge.historian_event.v2",
                "event_id": "event-1",
                "component": "manyforge.execution",
                "event_type": "operation_updated",
                "severity": "INFO",
                "occurred_at_ns": "1",
                "occurred_at_wall": "2026-08-28T00:00:00Z",
                "payload": {},
            },
            "sections": {
                name: {"available": False, "reason": "not published", "value": None}
                for name in ("incident", "execution_digest", "blackboard", "latency")
            },
        }

    def historian_health(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.historian_health.v1",
            "available": True,
        }

    def reconcile_historian_health(self, **kwargs: Any) -> dict[str, Any]:
        self.historian_reconciliations.append(kwargs)
        return {
            "schema_version": "manyforge.historian_reconciliation.v1",
            "reconciled": kwargs["expected_generation"] == 1,
            "audit_event_ids": ["event-1"],
            "health": self.historian_health(),
        }

    def shutdown(self) -> None:
        raise AssertionError("injected service must not be shut down by the app")


class FakeManager:
    def __init__(self) -> None:
        self.requests: list[ManagerApplyRequest] = []
        self.scene_requests: list[tuple[SceneTransactionRequest, str | None]] = []
        self.scene_pose_requests: list[SceneResourcePoseQuery] = []
        self.apply_observations: list[tuple[str, int]] = []

    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]:
        self.requests.append(request)
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "ok": False,
            "error": {
                "code": "kernel_owner_unavailable",
                "message": "operational Apply returns at I6/N",
            },
            "request_id": request.request_id,
            "request_owner_epoch": request.request_owner_epoch,
        }

    def observe_apply(self, apply_id: str, apply_generation: int) -> dict[str, Any]:
        self.apply_observations.append((apply_id, apply_generation))
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "apply_generation": apply_generation,
            "apply_id": apply_id,
            "candidate_projection_revision": None,
            "error": None,
            "last_progress": {
                "phase": "accepted",
                "record_digest": "sha256:" + "f" * 64,
            },
            "ok": True,
            "phase": "accepted",
            "prepared_activation": {
                "schema_version": "manyforge.prepared_activation_input.v5",
                "scenario_id": "scenario.1",
                "source_document_digest": "sha256:" + "1" * 64,
                "captured_input_digest": "sha256:" + "d" * 64,
                "deployment_artifact_id": "deployment.1",
                "deployment_revision": "sha256:" + "b" * 64,
                "retained_bundle_id": "sha256:" + "2" * 64,
                "graph_revision": "sha256:" + "3" * 64,
                "program_id": "program.1",
                "program_revision": "sha256:" + "4" * 64,
                "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "9" * 64},
                "node_environment_digest": "sha256:" + "5" * 64,
                "assistant_mode": "optional",
                "prepared_activation_id": "sha256:" + "e" * 64,
            },
            "request_id": "request.1",
            "request_owner_epoch": "client.epoch.1",
            "state": "pending",
        }

    def commit_scene_transaction(
        self,
        request: SceneTransactionRequest,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.scene_requests.append((request, trusted_principal_id))
        context = request.request_context
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "committed_projection_revision": {
                "retained_bundle_id": "sha256:" + "b" * 64,
                "graph_revision": "sha256:" + "c" * 64,
                "scene_revision": {
                    "scene_id": "sha256:" + "d" * 64,
                    "revision_id": 2,
                    "content_hash": "sha256:" + "c" * 64,
                },
            },
            "error": None,
            "ok": True,
            "projection_status": "committed",
            "replay_status": "accepted_new",
            "request_id": context.request_id,
            "request_owner_epoch": context.request_owner_epoch,
            "transaction_sequence": 1,
            "update_status": "geometry_changed",
        }

    def resolve_scene_resources(
        self,
        request: SceneResourcePoseQuery,
    ) -> dict[str, Any]:
        self.scene_pose_requests.append(request)
        return {
            "api_version": "manyforge.scene_resource_pose_response.v1",
            "poses": [
                {
                    "scene_resource_id": resource_id,
                    "pose_in_cell": {
                        "position_m": {"x": 0.1, "y": 0.2, "z": 0.3},
                        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                }
                for resource_id in request.scene_resource_ids
            ],
            "projection_revision": (request.expected_projection_revision.model_dump()),
        }


class FakeExecution:
    def __init__(self) -> None:
        self.requests: list[tuple[str, object]] = []
        self.principals: list[tuple[str, str | None]] = []

    def diagnostics(self) -> dict[str, Any]:
        return {"ok": True, "command": "status", "status": {}}

    def identity(self) -> dict[str, Any]:
        return {"ok": True, "command": "identity", "identity": {}}

    def targets(self) -> dict[str, Any]:
        return {"ok": True, "command": "targets", "targets": {}}

    def control_state(self) -> dict[str, Any]:
        return {"ok": True, "command": "control_state", "control_state": {}}

    def observation(self) -> dict[str, Any]:
        return {
            "api_version": "manyforge.execution_command_response.v1",
            "ok": True,
            "command": "observation",
            "observation": {"schema_version": "manyforge.execution_observation.v3"},
        }

    def submit(
        self,
        request: object,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(("submit", request))
        self.principals.append(("submit", trusted_principal_id))
        return {"ok": True, "command": "submit", "accepted": {}}

    def read_manual_handover(self, request, *, trusted_principal_id):
        self.requests.append(("read_manual_handover", request))
        self.principals.append(("read_manual_handover", trusted_principal_id))
        return {
            "ok": True,
            "command": "read_manual_handover",
            "handover": getattr(self, "manual_handover", None),
            "availability": {
                "available": True,
                "cleanup_required": False,
                "unavailable_reason": None,
            },
        }

    def preview_motion(
        self,
        request: object,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(("preview_motion", request))
        self.principals.append(("preview_motion", trusted_principal_id))
        return {
            "ok": True,
            "command": "preview_motion",
            "preview_receipt": {},
        }

    def preview_capability_command(
        self,
        request: object,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(("preview_capability_command", request))
        self.principals.append(("preview_capability_command", trusted_principal_id))
        return {
            "ok": True,
            "command": "preview_capability_command",
            "preview_receipt": {},
        }

    def read(self, operation_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "command": "read",
            "operation": {"operation_id": operation_id},
            "resident": True,
        }

    def plan_pick(
        self,
        request: object,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(("plan_pick", request))
        self.principals.append(("plan_pick", trusted_principal_id))
        return {"ok": True, "command": "plan_pick", "planning_result": {}}

    def plan_place(
        self,
        request: object,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(("plan_place", request))
        self.principals.append(("plan_place", trusted_principal_id))
        return {"ok": True, "command": "plan_place", "planning_result": {}}

    def _control(
        self,
        kind: str,
        request: object,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append((kind, request))
        self.principals.append((kind, trusted_principal_id))
        return {"ok": True, "command": kind, "accepted": {"control": kind}}

    def pause(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        return self._control("pause", request, trusted_principal_id)

    def resume(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        return self._control("resume", request, trusted_principal_id)

    def reset(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        return self._control("reset", request, trusted_principal_id)

    def acknowledge_completed(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        return self._control("acknowledge_completed", request, trusted_principal_id)

    def stop(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        self.requests.append(("stop", request))
        self.principals.append(("stop", trusted_principal_id))
        return {"ok": True, "command": "stop", "accepted": {}}

    def full_stop(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        self.requests.append(("full_stop", request))
        self.principals.append(("full_stop", trusted_principal_id))
        return {"ok": True, "command": "full_stop", "accepted": {}}

    def recognized_malformed_stop(
        self,
        *,
        request_id: str,
        recognized_route: str,
        reason: str,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(
            (
                "recognized_malformed_stop",
                {
                    "request_id": request_id,
                    "recognized_route": recognized_route,
                    "reason": reason,
                },
            )
        )
        self.principals.append(("recognized_malformed_stop", trusted_principal_id))
        return {
            "ok": True,
            "command": "stop",
            "accepted": {
                "request_id": request_id,
                "resolution_disposition": "promoted_to_full",
            },
        }

    def stop_fault(self) -> dict[str, Any]:
        self.requests.append(("stop_fault", None))
        return {"ok": True, "command": "get_stop_fault", "stop_fault": None}

    def recover_stop_fault(self, request: object) -> dict[str, Any]:
        self.requests.append(("recover_stop_fault", request))
        return {
            "ok": True,
            "command": "recover_stop_fault",
            "result": {"accepted": True},
        }

    def set_runtime_scale_control(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        self.requests.append(("set_runtime_scale_control", request))
        self.principals.append(("set_runtime_scale_control", trusted_principal_id))
        return {
            "ok": True,
            "command": "set_runtime_scale_control",
            "accepted": {},
        }

    def get_runtime_scale_control(self, selector: object) -> dict[str, Any]:
        self.requests.append(("get_runtime_scale_control", selector))
        return {
            "ok": True,
            "command": "get_runtime_scale_control",
            "runtime_scale_control": {},
        }


class FakeTrustedAdapter:
    def resolve(self, _request) -> TrustedPrincipal:
        return TrustedPrincipal("operator.7", "authenticated:test")


def _program_run_snapshot(*, state: str = "running") -> dict[str, Any]:
    digest_a = "sha256:" + "a" * 64
    digest_c = "sha256:" + "c" * 64
    program = _program_document()
    binding = _program_binding(
        program,
        retained_bundle_id=digest_c,
        graph_revision=digest_a,
    )
    return {
        "schema_version": "manyforge.program_run_status.v3",
        "owner_id": "program-runner.cell-a",
        "owner_epoch": "program-runner.epoch.1",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v2",
            "program_run_id": "program-run.1",
            "applied_record_digest": "sha256:" + "5" * 64,
            "scenario_id": "cell-pick",
            "source_document_digest": "sha256:" + "6" * 64,
            "program_id": program["program_id"],
            "program_revision": program["program_revision"],
            "program_binding_id": binding["program_binding_id"],
            "retained_bundle_id": digest_c,
            "graph_revision": digest_a,
            "node_environment_digest": binding["node_environment"][
                "environment_digest"
            ],
            "node_policy_digest": "sha256:4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945",
            "node_policies": [],
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


def _applied_for_program_run() -> dict[str, Any]:
    snapshot = _program_run_snapshot()
    capture = snapshot["capture"]
    return _applied_projection(
        _program_document(),
        retained_bundle_id=capture["retained_bundle_id"],
        graph_revision=capture["graph_revision"],
    )


class FakeProgramRunner:
    def __init__(
        self,
        *,
        active: bool = True,
        node_environment_digest: str | None = None,
    ) -> None:
        self.active = active
        self.node_environment_digest = node_environment_digest
        self.calls: list[tuple[str, Any]] = []

    def cycle_status(self) -> dict[str, Any]:
        command_kinds = (
            "program_run",
            "program_motion_stop",
            "program_full_stop",
            "program_stop",
            "program_reset",
            "program_resume_full_stop",
            "program_discard_full_stop",
            "program_runtime_blackboard_write",
            "program_manual_result_stage",
            "program_manual_handover",
            "operation_start",
            "operation_stop",
            "operation_pause",
            "operation_resume",
            "operation_reset",
            "operation_acknowledge_completed",
            "cell_full_stop",
            "execution_recovery",
            "stop_recovery",
            "runtime_scale",
        )
        return {
            "program_runner_owner_epoch": "program-runner.epoch.1",
            "execution_control_revision": 1,
            "cycle_generation": 1,
            "state": "idle_after_reset",
            "program_run_id": None,
            "allowed_commands": [
                {
                    "command_kind": kind,
                    "semantic_target": {},
                    "allowed": True,
                    "disabled_reason": None,
                }
                for kind in command_kinds
            ],
        }

    def node_availability(self, body: dict[str, Any]) -> dict[str, Any]:
        raise ProgramRunnerClientError(
            "program_node_availability_unavailable",
            "fixture does not publish node availability",
        )

    def _snapshot(self, *, state: str = "running") -> dict[str, Any]:
        snapshot = _program_run_snapshot(state=state)
        if self.node_environment_digest is not None:
            snapshot["capture"][
                "node_environment_digest"
            ] = self.node_environment_digest
        return snapshot

    def runs(self, *, active_only: bool, limit: int) -> dict[str, Any]:
        self.calls.append(("runs", (active_only, limit)))
        return {
            "schema_version": "manyforge.program_run_index.v1",
            "owner_id": "program-runner.cell-a",
            "owner_epoch": "program-runner.epoch.1",
            "active_only": active_only,
            "truncated": False,
            "runs": [self._snapshot()] if self.active else [],
        }

    def start_run(self, body: dict[str, Any]) -> dict[str, Any]:
        self.calls.append(("start", copy.deepcopy(body)))
        return self._snapshot()

    def prepare_applied_scene(self, body: dict[str, Any]) -> dict[str, Any]:
        self.calls.append(("prepare_scene", copy.deepcopy(body)))
        return {
            "schema_version": "manyforge.program_scene_preparation.v1",
            "status": "prepared",
            "applied_record_digest": body["expected_applied_record_digest"],
            "program_revision": self._snapshot()["capture"]["program_revision"],
            "admission_generation": body["expected_admission_generation"],
            "prepared_scene": {"schema_version": "manyforge.prepared_program_scene.v1"},
        }

    def read_run(self, program_run_id: str) -> dict[str, Any]:
        self.calls.append(("read", program_run_id))
        return self._snapshot()

    def stop_run(
        self,
        program_run_id: str,
        body: dict[str, Any],
    ) -> dict[str, Any]:
        self.calls.append(("stop", (program_run_id, copy.deepcopy(body))))
        return self._snapshot(state="stopping")

    def stop_motion(
        self,
        program_run_id: str,
        body: dict[str, Any],
    ) -> dict[str, Any]:
        self.calls.append(("motion_stop", (program_run_id, copy.deepcopy(body))))
        return self._snapshot(state="running")

    def full_stop(
        self,
        program_run_id: str,
        body: dict[str, Any],
    ) -> dict[str, Any]:
        self.calls.append(("full_stop", (program_run_id, copy.deepcopy(body))))
        return self._snapshot(state="full_stopping")

    def prepare_manual_handover(
        self, program_run_id: str, body: dict[str, Any]
    ) -> dict[str, Any]:
        self.calls.append(("manual_handover", (program_run_id, copy.deepcopy(body))))
        return self._snapshot()

    def resume_full_stop(
        self,
        program_run_id: str,
        body: dict[str, Any],
    ) -> dict[str, Any]:
        self.calls.append(("resume_full_stop", (program_run_id, copy.deepcopy(body))))
        return self._snapshot(state="running")

    def discard_full_stop(
        self,
        program_run_id: str,
        body: dict[str, Any],
    ) -> dict[str, Any]:
        self.calls.append(("discard_full_stop", (program_run_id, copy.deepcopy(body))))
        return self._snapshot(state="stopped")

    def close(self) -> None:
        self.calls.append(("close", None))


class _NodeAvailabilityRunner(FakeProgramRunner):
    def __init__(self, projection: dict[str, Any]) -> None:
        super().__init__()
        self.projection = projection

    def node_availability(self, body: dict[str, Any]) -> dict[str, Any]:
        self.calls.append(("node_availability", copy.deepcopy(body)))
        return copy.deepcopy(self.projection)


def _node_availability_fixture(
    applied: dict[str, Any],
) -> tuple[dict[str, Any], dict[str, Any]]:
    environment = build_node_environment()
    resolver = build_executable_node_resolver(environment)
    dispositions = resolver.dispositions
    readiness = NodeExecutionReadiness(
        program_binding_id=applied["program_binding_id"],
        binding_by_node={
            disposition.row.node_id: AvailabilityFact(True)
            for disposition in dispositions
        },
        owner_by_port={
            disposition.owner_port_name: AvailabilityFact(True)
            for disposition in dispositions
            if disposition.owner_port_name is not None
        },
    )
    required: set[str] = set()

    def collect(node: dict[str, Any]) -> None:
        required.add(node["node_id"])
        for child in node.get("children", []):
            collect(child)

    collect(applied["program"]["tree"])
    availability = project_node_execution_availability(
        resolver,
        readiness,
        required_node_ids=tuple(sorted(required)),
    )
    core_implementations = []
    for row in availability["implementations"]:
        core = {
            key: copy.deepcopy(value)
            for key, value in row.items()
            if key != "owner_port_name"
        }
        core["binding_available"] = False
        core["execution_available"] = False
        core["reason_codes"] = ["program_binding_unselected"]
        core_implementations.append(core)
    core_projection = {
        "schema_version": "manyforge.node_environment_projection.v2",
        "retained_bundle_id": applied["retained_bundle_id"],
        "graph_revision": applied["graph_revision"],
        "environment_digest": environment.manifest.environment_digest,
        "availability": {
            "binding_context": "unselected",
            "execution_available": False,
            "implementations": core_implementations,
            "manager_runtime_converged": True,
            "portable_authoring_available": True,
            "reason_codes": ["program_binding_unselected"],
            "restoration_gates": [],
        },
        "source": {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
        },
    }
    source = {
        "admission_generation": 7,
        "applied_record_digest": applied["source"]["record_digest"],
        "cycle_generation": 11,
        "graph_revision": applied["graph_revision"],
        "node_environment_digest": environment.manifest.environment_digest,
        "owner_epoch": "program-runner.epoch.1",
        "owner_id": "program-runner",
        "program_binding_id": applied["program_binding_id"],
        "program_id": applied["program_id"],
        "program_revision": applied["program_revision"],
        "retained_bundle_id": applied["retained_bundle_id"],
        "scenario_id": applied["scenario_id"],
    }
    runner_projection = {
        "schema_version": "manyforge.program_node_availability.v1",
        "availability": availability,
        "source": {
            **source,
            "content_digest": "sha256:"
            + hashlib.sha256(
                rfc8785.dumps({"availability": availability, **source})
            ).hexdigest(),
        },
    }
    return core_projection, runner_projection


class _ResetProgramRunner(FakeProgramRunner):
    def __init__(self) -> None:
        super().__init__()
        self.cycle = {
            "program_runner_owner_epoch": "program-runner.epoch.1",
            "cycle_generation": 4,
            "state": "terminal_awaiting_reset",
            "program_run_id": "program-run.1",
            "cycle_claim_generation": 22,
            "terminal_or_settlement_reference": "sha256:" + "a" * 64,
        }

    def cycle_status(self) -> dict[str, Any]:
        self.calls.append(("cycle_status", None))
        return copy.deepcopy(self.cycle)

    def reset_cycle(self, body: dict[str, Any]) -> dict[str, Any]:
        self.calls.append(("reset_cycle", copy.deepcopy(body)))
        if (
            self.cycle["state"] not in {"running", "terminal_awaiting_reset"}
            or self.cycle["cycle_generation"] != body["expected_cycle_generation"]
        ):
            raise ProgramRunnerClientError(
                "program_cycle_reset_stale",
                "Reset does not identify the current Program cycle",
                409,
            )
        return {
            "schema_version": "manyforge.program_cycle_reset_result.v1",
            "program_run_id": body["program_run_id"],
            "reset_cycle_generation": body["expected_cycle_generation"],
            "idle_cycle_generation": body["expected_cycle_generation"] + 1,
            "settlement_reference": body["settlement_evidence_digest"],
            "admission": {},
            "runtime_controls": None,
        }


def _cycle_reset_request() -> ProgramCycleResetRequest:
    return ProgramCycleResetRequest.model_validate(
        {
            "request_id": "reset.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "composer",
            "correlation_id": None,
            "program_run_id": "program-run.1",
            "expected_owner_epoch": "program-runner.epoch.1",
            "expected_cycle_generation": 4,
            "expected_claim_generation": 22,
            "terminal_reference": "sha256:" + "a" * 64,
            "settlement_evidence_digest": "sha256:" + "a" * 64,
        }
    )


def test_program_cycle_reset_is_one_thin_owner_command() -> None:
    execution = FakeExecution()
    runner = _ResetProgramRunner()
    service = _service(
        FakeCore(), FakeManager(), execution=execution, program_runner=runner
    )

    result = service.program_cycle_reset(_cycle_reset_request())

    assert result["idle_cycle_generation"] == 5
    assert execution.requests == []
    assert [kind for kind, _request in runner.calls] == ["reset_cycle"]


def test_stale_program_cycle_reset_has_no_stop_recovery_effect() -> None:
    execution = FakeExecution()
    runner = _ResetProgramRunner()
    runner.cycle["cycle_generation"] = 5
    service = _service(
        FakeCore(), FakeManager(), execution=execution, program_runner=runner
    )

    with pytest.raises(HeadlessApiError) as stale:
        service.program_cycle_reset(_cycle_reset_request())

    assert stale.value.code == "program_cycle_reset_stale"
    assert execution.requests == []
    assert [kind for kind, _request in runner.calls] == ["reset_cycle"]


def test_successor_idle_reset_is_rejected_without_replaying_effects() -> None:
    execution = FakeExecution()
    runner = _ResetProgramRunner()
    runner.cycle.update(
        {
            "cycle_generation": 5,
            "state": "idle_after_reset",
            "program_run_id": None,
            "cycle_claim_generation": None,
            "terminal_or_settlement_reference": None,
        }
    )
    service = _service(
        FakeCore(), FakeManager(), execution=execution, program_runner=runner
    )

    with pytest.raises(HeadlessApiError) as stale:
        service.program_cycle_reset(_cycle_reset_request())

    assert stale.value.code == "program_cycle_reset_stale"
    assert execution.requests == []
    assert [kind for kind, _request in runner.calls] == ["reset_cycle"]


def test_full_stopped_reset_is_one_public_command_with_exact_inner_settlement() -> None:
    execution = FakeExecution()
    runner = _ResetProgramRunner()
    runner.cycle.update(
        {
            "state": "running",
            "terminal_or_settlement_reference": None,
        }
    )
    service = _service(
        FakeCore(), FakeManager(), execution=execution, program_runner=runner
    )
    request = ProgramFullStopResetRequest.model_validate(
        {
            "request_id": "reset-full-stop.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "composer",
            "correlation_id": None,
            "program_run_id": "program-run.1",
            "expected_owner_epoch": "program-runner.epoch.1",
            "expected_run_revision": 1,
            "expected_cycle_generation": 4,
            "expected_claim_generation": 22,
            "reason": "operator Reset discarded Full Stop",
        }
    )

    result = service.program_run_reset_full_stop("program-run.1", request)

    assert result["idle_cycle_generation"] == 5
    assert [kind for kind, _value in runner.calls] == ["reset_cycle"]
    reset_body = runner.calls[-1][1]
    assert reset_body["terminal_reference"] is None
    assert reset_body["settlement_evidence_digest"] is None
    assert reset_body["request_id"] == request.request_id
    assert execution.requests == []


def _service(
    core: FakeCore,
    manager: FakeManager,
    execution: object | None = None,
    program_runner: object | None = None,
    cclc: object | None = None,
    activation_preparer: object | None = None,
) -> HeadlessService:
    return HeadlessService(
        core,
        manager,
        execution=execution,  # type: ignore[arg-type]
        program_runner=program_runner,  # type: ignore[arg-type]
        cell_admission=FakeCellAdmission(),
        cclc=cclc,
        activation_preparer=activation_preparer,  # type: ignore[arg-type]
    )


def _client(*, trusted: bool = False) -> tuple[TestClient, FakeCore, FakeManager]:
    core = FakeCore()
    manager = FakeManager()
    app = create_app(
        service=_service(core, manager),
        trusted_principal_adapter=FakeTrustedAdapter() if trusted else None,
    )
    return TestClient(app), core, manager


class FakeActivationPreparer:
    def __init__(self) -> None:
        self.calls: list[dict[str, Any]] = []

    def prepare(self, **kwargs: Any) -> dict[str, Any]:
        self.calls.append(copy.deepcopy(kwargs))
        return {
            "schema_version": "manyforge.prepared_activation_input.v5",
            "source_document_digest": kwargs["source_document_digest"],
            "scenario_id": kwargs["scenario_id"],
            "captured_input_digest": kwargs["captured_input_digest"],
            "prepared_activation_id": "sha256:" + "6" * 64,
            "deployment_artifact_id": "cell_a",
            "deployment_revision": "sha256:" + "1" * 64,
            "retained_bundle_id": "sha256:" + "2" * 64,
            "graph_revision": "sha256:" + "3" * 64,
            "program_id": "program_a",
            "program_revision": "sha256:" + "4" * 64,
            "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "5" * 64},
            "node_environment_digest": "sha256:" + "7" * 64,
            "assistant_mode": "forbidden",
        }


def test_activation_preparation_requires_current_idle_generation() -> None:
    core = FakeCore()
    manager = FakeManager()
    preparer = FakeActivationPreparer()
    client = TestClient(
        create_app(service=_service(core, manager, activation_preparer=preparer))
    )
    body = {
        "scenario_id": "scenario_a",
        "source_document_digest": "sha256:" + "6" * 64,
        "captured_input_digest": "sha256:" + "8" * 64,
        "deployment": {"api_version": "manyforge.deployment.v1"},
        "model_source_closure": {
            "schema_version": "manyforge.activation_model_source_closure.v1",
            "components": [],
        },
        "program": {"api_version": "manyforge.program.v2"},
        "program_role_assignment": {"component_roles": [], "target_roles": []},
        "expected_admission_generation": _ADMISSION_GENERATION,
    }

    response = client.post("/api/activation-preparations", json=body)

    assert response.status_code == 200, response.text
    assert response.json()["prepared_activation"]["prepared_activation_id"]
    assert preparer.calls == [
        {
            "scenario_id": "scenario_a",
            "source_document_digest": "sha256:" + "6" * 64,
            "captured_input_digest": "sha256:" + "8" * 64,
            "deployment": {"api_version": "manyforge.deployment.v1"},
            "model_source_closure": {
                "schema_version": "manyforge.activation_model_source_closure.v1",
                "components": [],
            },
            "program": {"api_version": "manyforge.program.v2"},
            "program_role_assignment": {"component_roles": [], "target_roles": []},
        }
    ]

    body["expected_admission_generation"] = _ADMISSION_GENERATION + 1
    rejected = client.post("/api/activation-preparations", json=body)
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == "stale_generation"


def test_activation_preparation_refuses_removed_parameter_overrides() -> None:
    preparer = FakeActivationPreparer()
    client = TestClient(
        create_app(
            service=_service(FakeCore(), FakeManager(), activation_preparer=preparer)
        )
    )
    body = {
        "scenario_id": "scenario_a",
        "source_document_digest": "sha256:" + "6" * 64,
        "captured_input_digest": "sha256:" + "8" * 64,
        "deployment": {"api_version": "manyforge.deployment.v1"},
        "model_source_closure": {
            "schema_version": "manyforge.activation_model_source_closure.v1",
            "components": [],
        },
        "program": {"api_version": "manyforge.program.v2"},
        "program_role_assignment": {"component_roles": [], "target_roles": []},
        "typed_parameter_overrides": {},
        "expected_admission_generation": _ADMISSION_GENERATION,
    }

    response = client.post("/api/activation-preparations", json=body)

    assert response.status_code == 422
    assert "typed_parameter_overrides was removed" in response.text
    assert preparer.calls == []


class FakeCCLC:
    def __init__(self) -> None:
        self.routes: list[dict[str, Any]] = []

    def submit_route(self, request: dict[str, Any]) -> dict[str, Any]:
        self.routes.append(copy.deepcopy(request))
        return {
            "transaction_id": "route.1",
            "replay_status": "accepted_new",
            "admission": None,
        }


def _route_selection_body(admission_generation: int) -> dict[str, Any]:
    return {
        "request_context": _caller(),
        "expected_admission_generation": admission_generation,
        "expected_retained_bundle_id": "bundle.1",
        "expected_graph_revision": "graph.1",
        "desired_configuration": {
            "control_domain_id": "domain.arm",
            "control_domain_revision": "domain.revision.1",
            "controller_topology_id": "topology.arm",
            "topology_revision": "topology.revision.1",
            "activation_set_id": "activation.arm",
            "binding_routes": [
                {
                    "control_binding_id": "binding.arm",
                    "control_binding_revision": "binding.revision.1",
                    "control_route_id": "route.normal",
                }
            ],
        },
        "expected_active_control_configuration": {
            "presence": "absent",
            "active_control_configuration_id": None,
            "active_control_configuration_generation": None,
        },
        "component_expectations": [
            {
                "controllable_component_id": "component.arm",
                "expected_component_lifecycle_generation": 1,
                "expected_component_lifecycle_state": "prepared",
            }
        ],
        "confirmed": True,
    }


def test_route_selection_preserves_admission_for_core_without_an_idle_gate() -> None:
    core = FakeCore()
    manager = FakeManager()
    cclc = FakeCCLC()
    service = _service(core, manager, cclc=cclc)
    service.authoring_admission = lambda: pytest.fail(
        "HTTP must not decide route admission"
    )
    app = create_app(
        service=service,
        cclc=cclc,  # type: ignore[arg-type]
    )
    with TestClient(app) as client:
        stale = client.post(
            "/api/cclc/routes/requests",
            json=_route_selection_body(_ADMISSION_GENERATION + 1),
        )
        accepted = client.post(
            "/api/cclc/routes/requests",
            json=_route_selection_body(_ADMISSION_GENERATION),
        )

    # The thin boundary forwards both; the real Core claim owner, not HTTP,
    # decides whether either generation is still admissible.
    assert stale.status_code == 200
    assert accepted.status_code == 200
    assert len(cclc.routes) == 2
    assert cclc.routes[0]["expected_admission_generation"] == _ADMISSION_GENERATION + 1
    assert cclc.routes[1]["expected_admission_generation"] == _ADMISSION_GENERATION


@pytest.mark.parametrize(
    "mutation",
    [
        lambda context: context.update(
            {"schema_version": "manyforge.source_model_context.v0"}
        ),
        lambda context: context.update({"extra": True}),
        lambda context: context.pop("source_model"),
        lambda context: context.update({"source_model": []}),
    ],
)
def test_source_model_context_rejects_closed_or_malformed_envelopes(mutation) -> None:
    client, core, _manager = _client()
    context = core.source_model_context()
    mutation(context)
    core.source_model_context_override = context

    with client:
        response = client.get("/api/source-model-context")

    assert response.status_code == 502
    assert response.json()["detail"]["code"] == "source_model_context_invalid"


def _caller() -> dict[str, Any]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "client.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": "correlation.1",
    }


def test_runtime_manager_scene_alarm_history_and_health_are_thin_reads() -> None:
    client, _core, _manager = _client()
    with client:
        assert client.get("/api/runtime").json()["complete"] is True
        assert client.get("/api/manager/graph").json()["graph_revision"] == "graph.1"
        assert client.get("/api/manager/status").json()["status"]["manager_ready"]
        assert client.get("/api/manager/diff").json()["converged"]
        environment = client.get("/api/node-environment").json()
        assert environment["retained_bundle_id"] == "bundle.1"
        assert environment["source"]["source_owner_id"] == (
            "headless-api.node-environment"
        )
        source_model = client.get("/api/source-model").json()
        assert source_model["retained_bundle_id"] == "bundle.1"
        assert source_model["occurrences"] == []
        assert source_model["source"]["source_owner_id"] == (
            "headless-api.source-model"
        )
        context = client.get("/api/source-model-context").json()
        assert context["schema_version"] == "manyforge.source_model_context.v1"
        assert context["node_environment"]["schema_version"] == (
            "manyforge.node_environment_projection.v2"
        )
        assert context["source_model"]["schema_version"] == (
            "manyforge.source_model_projection.v2"
        )
        urdf = client.get("/api/viewer-model/urdf")
        assert urdf.status_code == 200
        assert urdf.content == b"<robot name='fixture'/>"
        assert urdf.headers["content-type"] == "application/xml"
        assert urdf.headers["cache-control"] == "no-store"
        assert urdf.headers["x-manyforge-product-id"] == "cell_viewer_model"

        bundle_id = "sha256:" + "1" * 64
        logical_uri = "asset://fixture/revision/model.stl"
        resource = client.get(
            "/api/viewer-model/resource",
            params={
                "retained_bundle_id": bundle_id,
                "logical_uri": logical_uri,
            },
        )
        assert resource.status_code == 200
        assert resource.headers["cache-control"] == (
            "public, max-age=31536000, immutable"
        )
        assert resource.content == b"solid"
        assert resource.headers["content-type"] == "model/stl"
        assert resource.headers["x-manyforge-retained-bundle-id"] == bundle_id
        assistant_modes = client.get("/api/assistant/modes").json()
        assert [row["mode"] for row in assistant_modes["modes"]] == [
            "composer-assistant"
        ]
        assistant_mode = client.get("/api/assistant/modes/composer-assistant").json()
        assert assistant_mode["mode"] == "composer-assistant"
        assert assistant_mode["retained_bundle_id"] == "bundle.1"
        assistant_context = client.get(
            "/api/assistant/modes/composer-assistant/context"
        ).json()
        assert assistant_context["schema_version"] == (
            "manyforge.assistant_mode_context.v1"
        )
        assert assistant_context["assistant_mode"]["mode"] == ("composer-assistant")
        assert client.get("/api/scene").status_code == 200
        assert client.get("/api/execution/alarms").json()["requested_limit"] == 32
        alarms = client.get(
            "/api/execution/alarms",
            params=[
                ("limit", "2"),
                ("states", "active_unacknowledged"),
                ("ordering", "attention_newest"),
            ],
        ).json()
        assert alarms["requested_states"] == ["active_unacknowledged"]
        assert alarms["requested_ordering"] == "attention_newest"
        assert client.get("/api/execution/history?limit=3").status_code == 200
        health = client.get("/api/health").json()
        assert health["status_scope"] == "headless_projection"
        assert health["headless_projection_ready"] is True
        assert "ready" not in health
        assert "robot_readiness" not in health
        overview = client.get("/api/component-overview").json()
        assert overview["schema_version"] == "manyforge.component_overview.v2"
        assert overview["readiness"] == "degraded"
        assert overview["overview_revision"].startswith("sha256:")
        assert (
            next(
                row for row in overview["components"] if row["id"] == "observe_render"
            )["status"]
            == "healthy"
        )


def test_component_overview_fact_capture_reads_each_source_once() -> None:
    class CountingFactService:
        def __init__(self) -> None:
            self.calls: list[str] = []

        def value(self, name: str) -> dict[str, Any]:
            self.calls.append(name)
            return {"source": name}

        def runtime(self) -> dict[str, Any]:
            return self.value("runtime")

        def alarms(self, **kwargs: Any) -> dict[str, Any]:
            assert kwargs == {"limit": 32, "ordering": "attention_newest"}
            return self.value("alarms")

        def authoring_admission(self) -> dict[str, Any]:
            return self.value("authoring_admission")

        def program_cycle_status(self) -> dict[str, Any]:
            return self.value("program_cycle")

        def program_applied(self) -> dict[str, Any]:
            return self.value("applied_program")

        def source_model(self) -> dict[str, Any]:
            return self.value("source_model")

        def execution_identity(self) -> dict[str, Any]:
            return self.value("execution_identity")

        def execution_targets(self) -> dict[str, Any]:
            return self.value("execution_targets")

        def execution_control_state(self) -> dict[str, Any]:
            return self.value("execution_control")

    service = CountingFactService()
    exact_cycle = {"source": "program_stream"}
    facts = HeadlessService.component_overview_facts(  # type: ignore[arg-type]
        service,
        program_cycle=exact_cycle,
    )

    assert facts.program_cycle is exact_cycle
    assert service.calls == [
        "runtime",
        "alarms",
        "authoring_admission",
        "applied_program",
        "source_model",
        "execution_identity",
        "execution_targets",
        "execution_control",
    ]
    assert len(service.calls) == len(set(service.calls)) == 8


def test_node_environment_joins_only_the_exact_applied_runner_offer() -> None:
    core = FakeCore()
    applied = _applied_projection(_program_document())
    core.applied_program = copy.deepcopy(applied)
    core_projection, runner_projection = _node_availability_fixture(applied)
    core.node_environment_projection = lambda: copy.deepcopy(core_projection)  # type: ignore[method-assign]
    runner = _NodeAvailabilityRunner(runner_projection)
    service = _service(core, FakeManager(), program_runner=runner)

    projected = service.node_environment()

    assert projected["availability"]["binding_context"] == "applied"
    assert projected["availability"]["execution_available"] is True
    required = set(runner_projection["availability"]["required_node_ids"])
    used = {
        row["node_id"]: row
        for row in projected["availability"]["implementations"]
        if row["node_id"] in required
    }
    assert set(used) == required
    assert all(row["execution_available"] for row in used.values())
    assert runner.calls == [
        (
            "node_availability",
            {
                "applied": applied,
            },
        )
    ]


def test_node_environment_rejects_a_stale_runner_applied_identity() -> None:
    core = FakeCore()
    applied = _applied_projection(_program_document())
    core.applied_program = copy.deepcopy(applied)
    core_projection, runner_projection = _node_availability_fixture(applied)
    core.node_environment_projection = lambda: copy.deepcopy(core_projection)  # type: ignore[method-assign]
    runner_projection["source"]["program_revision"] = "sha256:" + "f" * 64
    source = {
        key: value
        for key, value in runner_projection["source"].items()
        if key != "content_digest"
    }
    runner_projection["source"]["content_digest"] = (
        "sha256:"
        + hashlib.sha256(
            rfc8785.dumps(
                {
                    "availability": runner_projection["availability"],
                    **source,
                }
            )
        ).hexdigest()
    )
    service = _service(
        core,
        FakeManager(),
        program_runner=_NodeAvailabilityRunner(runner_projection),
    )

    with pytest.raises(HeadlessApiError) as stale:
        service.node_environment()

    assert stale.value.code == "program_node_availability_stale"


def test_manager_status_and_diff_preserve_offer_digest_fields() -> None:
    client, _core, _manager = _client()
    with client:
        status = client.get("/api/manager/status").json()
        diff = client.get("/api/manager/diff").json()

    desired = status["status"]["runtime"]["desired_units"]
    observed = status["status"]["runtime"]["observed_units"]
    assert desired[0]["selected_runtime_contract_offer_digest"] == (
        "sha256:" + "c" * 64
    )
    assert observed[0]["observed_runtime_contract_offer_digest"] == (
        "sha256:" + "c" * 64
    )
    assert diff["desired_units"] == desired
    assert diff["observed_units"] == observed


def test_program_runner_surface_is_a_strict_thin_owner_projection() -> None:
    runner = FakeProgramRunner()
    core = FakeCore()
    core.applied_program = _applied_for_program_run()
    app = create_app(
        service=_service(
            core,
            FakeManager(),
            program_runner=runner,
        )
    )
    client = TestClient(app)
    start_body = {
        **_caller(),
        "scenario_id": "cell-pick",
        "source_document_digest": "sha256:" + "6" * 64,
        "expected_applied_record_digest": "sha256:" + "5" * 64,
        "draft_program_revision": core.applied_program["program_revision"],
        "expected_admission_generation": _ADMISSION_GENERATION,
    }
    stop_body = {
        **_caller(),
        "request_id": "request.2",
        "program_run_id": "program-run.1",
        "expected_owner_epoch": "program-runner.epoch.1",
        "reason": "operator stop",
    }

    with client:
        index = client.get(
            "/api/program/runs",
            params={"active_only": "true", "limit": 1},
        )
        started = client.post("/api/program/runs", json=start_body)
        prepared = client.post(
            "/api/program/scene-preparations",
            json={
                **_caller(),
                "request_id": "request.prepare.1",
                "expected_applied_record_digest": "sha256:" + "5" * 64,
                "expected_admission_generation": _ADMISSION_GENERATION,
            },
        )
        read = client.get("/api/program/runs/program-run.1")
        motion_stopped = client.post(
            "/api/program/runs/program-run.1/motion-stop",
            json={
                **stop_body,
                "request_id": "request.motion-stop.1",
                "reason": "ordinary operator motion Stop",
            },
        )
        full_stopped = client.post(
            "/api/program/runs/program-run.1/full-stop",
            json={
                **stop_body,
                "request_id": "request.full-stop.1",
                "reason": "ordinary operator Full Stop",
            },
        )
        resumed = client.post(
            "/api/program/runs/program-run.1/resume-full-stop",
            json={
                **_caller(),
                "request_id": "request.resume-full-stop.1",
                "program_run_id": "program-run.1",
                "expected_owner_epoch": "program-runner.epoch.1",
                "expected_run_revision": 1,
                "reason": "operator Run after Full Stop",
            },
        )
        stopped = client.post(
            "/api/program/runs/program-run.1/stop",
            json=stop_body,
        )

        assert index.status_code == 200
        assert index.json()["active_only"] is True
        assert started.status_code == 200, started.text
        assert started.json()["capture"]["scenario_id"] == "cell-pick"
    assert prepared.json()["status"] == "prepared"
    assert read.json()["capture"]["program_run_id"] == "program-run.1"
    assert motion_stopped.json()["state"] == "running"
    assert full_stopped.json()["state"] == "full_stopping"
    assert resumed.json()["state"] == "running"
    assert stopped.json()["state"] == "stopping"
    assert [call[0] for call in runner.calls] == [
        "runs",
        "start",
        "prepare_scene",
        "read",
        "motion_stop",
        "full_stop",
        "resume_full_stop",
        "stop",
    ]


def test_non_executable_program_refuses_execute_but_not_manual_control() -> None:
    """Program Execute depends only on its own binding outcome (H1a).

    A Program whose referenced target was removed is refused by name for
    Execute, while manual control of an installed component is unaffected.
    """

    class NonExecutableCore(FakeCore):
        def applied_program_selection(self) -> dict[str, Any]:
            raise HeadlessApiError(
                "program_applied_non_executable",
                "Applied Program is not executable in the active Deployment: "
                "target role 'gripper' (component 'lite6_gripper', target "
                "'gripper') is component_absent",
            )

    core = NonExecutableCore()
    runner = FakeProgramRunner()
    execution = FakeExecution()
    service = _service(
        core, FakeManager(), execution=execution, program_runner=runner
    )
    client = TestClient(create_app(service=service))
    start_body = {
        **_caller(),
        "scenario_id": "cell-pick",
        "source_document_digest": "sha256:" + "6" * 64,
        "expected_applied_record_digest": "sha256:" + "5" * 64,
        "draft_program_revision": "sha256:" + "7" * 64,
        "expected_admission_generation": _ADMISSION_GENERATION,
    }

    with client:
        refused = client.post("/api/program/runs", json=start_body)

    assert refused.status_code == 409, refused.text
    assert refused.json()["detail"]["code"] == "program_applied_non_executable"
    assert "'gripper'" in refused.json()["detail"]["message"]
    assert [call[0] for call in runner.calls if call[0] == "start"] == []

    # Manual control never consults the Applied Program outcome.
    manual_request = object()
    accepted = service.execution_submit(
        manual_request,  # type: ignore[arg-type]
        TrustedAttribution(principal_id="operator.1"),
    )
    assert accepted["ok"] is True
    assert execution.requests == [("submit", manual_request)]


def test_program_run_captures_current_environment_not_package_provenance() -> None:
    current_environment = "sha256:" + "e" * 64
    core = FakeCore()
    core.applied_program = _applied_for_program_run()
    core.node_environment_digest = current_environment
    runner = FakeProgramRunner(node_environment_digest=current_environment)
    client = TestClient(
        create_app(service=_service(core, FakeManager(), program_runner=runner))
    )
    source = core.applied_program["source"]
    request = {
        **_caller(),
        "scenario_id": "cell-pick",
        "source_document_digest": "sha256:" + "6" * 64,
        "expected_applied_record_digest": source["record_digest"],
        "draft_program_revision": core.applied_program["program_revision"],
        "expected_admission_generation": _ADMISSION_GENERATION,
    }

    response = client.post("/api/program/runs", json=request)

    assert response.status_code == 200
    assert response.json()["capture"]["node_environment_digest"] == (
        current_environment
    )
    assert current_environment != core.applied_program["node_environment_digest"]


def test_program_run_status_carries_failure_details_and_origin_unchanged() -> None:
    failed = {
        **_program_run_snapshot(state="failed"),
        "reason_code": "owner_refused",
        "message": "Kernel refused move: target not ready",
        "details": {
            "reason": "target_not_ready",
            "reference_type": "frame",
            "reference_id": "r" * 256,
            "retryable": False,
            "latch_id": None,
        },
        "origin": {
            "kind": "owner",
            "node_instance_id": "move",
            "owner_api": "kernel.operation",
        },
    }
    expected = copy.deepcopy(failed)
    assert HeadlessService._validate_program_run_snapshot(failed) == expected
    for kind, node, owner_api in (
        ("program_node", "wait", None),
        ("owner", "move", None),
        ("program_runner", None, None),
        ("program_runner", "move", None),
    ):
        HeadlessService._validate_program_run_snapshot(
            {
                **failed,
                "origin": {
                    "kind": kind,
                    "node_instance_id": node,
                    "owner_api": owner_api,
                },
            }
        )
    HeadlessService._validate_program_run_snapshot({**failed, "details": None})

    def origin(kind: Any, node: Any, owner_api: Any) -> dict[str, Any]:
        return {"kind": kind, "node_instance_id": node, "owner_api": owner_api}

    oversized = {
        key: "x" * 250
        for key in (
            "input_problem",
            "path",
            "reason",
            "reference_id",
            "reference_type",
            "required_action",
            "source_kind",
            "source_reference",
            "target_role_id",
        )
    }
    for field, value in (
        ("schema_version", "manyforge.program_run_status.v2"),
        ("details", {"unregistered": "x"}),
        ("details", {"reason": "x" * 257}),
        ("details", {"reason": {"nested": True}}),
        ("details", {"reason": ["listed"]}),
        ("details", {"reason": float("inf")}),
        ("details", {"reason": 2**53}),
        ("details", oversized),
        ("details", {}),
        ("details", "target_not_ready"),
        ("origin", None),
        ("origin", "owner"),
        ("origin", origin("kernel", "move", None)),
        ("origin", origin("program_node", None, None)),
        ("origin", origin("program_node", "move", "kernel.operation")),
        ("origin", origin("owner", None, "kernel.operation")),
        ("origin", origin("program_runner", None, "kernel.operation")),
        ("origin", origin("owner", "n" * 257, None)),
        ("origin", origin("owner", "move", "")),
        ("origin", {"kind": "program_runner", "node_instance_id": None}),
    ):
        with pytest.raises(HeadlessApiError) as error:
            HeadlessService._validate_program_run_snapshot({**failed, field: value})
        assert error.value.code == "program_runner_response_invalid"
    without_origin = {key: value for key, value in failed.items() if key != "origin"}
    with pytest.raises(HeadlessApiError):
        HeadlessService._validate_program_run_snapshot(without_origin)
    with pytest.raises(HeadlessApiError):
        HeadlessService._validate_program_run_snapshot(
            {**failed, "state": "succeeded", "details": None}
        )


def test_program_run_start_rejects_stale_applied_or_divergent_runner() -> None:
    core = FakeCore()
    core.applied_program = _applied_for_program_run()
    runner = FakeProgramRunner()
    client = TestClient(
        create_app(
            service=_service(
                core,
                FakeManager(),
                program_runner=runner,
            )
        )
    )
    body = {
        **_caller(),
        "scenario_id": "cell-pick",
        "source_document_digest": "sha256:" + "6" * 64,
        "expected_applied_record_digest": "sha256:" + "5" * 64,
        "draft_program_revision": core.applied_program["program_revision"],
        "expected_admission_generation": _ADMISSION_GENERATION,
    }

    with client:
        stale = client.post(
            "/api/program/runs",
            json={
                **body,
                "expected_applied_record_digest": "sha256:" + "f" * 64,
            },
        )
        original = runner.start_run

        def divergent(request: dict[str, Any]) -> dict[str, Any]:
            snapshot = original(request)
            snapshot["capture"]["program_revision"] = "sha256:" + "f" * 64
            return snapshot

        runner.start_run = divergent  # type: ignore[method-assign]
        divergent_response = client.post("/api/program/runs", json=body)

    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == ("program_run_applied_selection_stale")
    assert divergent_response.status_code == 502
    assert divergent_response.json()["detail"]["code"] == (
        "program_runner_response_invalid"
    )


def test_program_model_attempt_is_validated_before_native_commit() -> None:
    core = FakeCore()
    app = create_app(service=_service(core, FakeManager()))
    client = TestClient(app)
    digest = "sha256:" + "a" * 64
    body = {
        "schema_version": "manyforge.program_model_attempt.v1",
        "model_attempt_id": "attempt.1",
        "program_run_id": "program-run.1",
        "node_instance_id": "query.1",
        "node_attempt_id": "node-attempt.1",
        "external_attempt_index": 1,
        "assistant_projection_digest": digest,
        "model_endpoint_alias": "selection",
        "external_resource_alias": "site_llm",
        "model_id": "model-a",
        "prompt_template_id": "select-object",
        "prompt_template_version": "v1",
        "prompt_template_digest": digest,
        "prompt_digest": digest,
        "argument_digests": {"objects": digest},
        "attachment_digest": None,
        "audit_policy": {
            "redact_argument_values": True,
            "store_full_prompts": False,
            "store_full_responses": False,
        },
        "prompt_text": None,
        "argument_values": None,
        "outcome": "timeout",
        "error_code": "model_timeout",
        "error_message": "deadline reached",
        "response_digest": None,
        "validated_output_digest": None,
        "model_version": "",
        "occurred_at_ns": 123,
        "occurred_at_wall": "2026-08-10T00:00:00Z",
        "latency_ms": 30_000.0,
        "usage": None,
        "response_value": None,
    }

    with client:
        accepted = client.post("/api/program/model-attempts", json=body)
        invalid = client.post(
            "/api/program/model-attempts",
            json={
                **body,
                "audit_policy": {
                    **body["audit_policy"],
                    "invented": True,
                },
            },
        )

    assert accepted.status_code == 200
    assert accepted.json()["status"] == "stored"
    assert invalid.status_code == 422
    assert core.model_attempts == [body]


def test_acknowledgement_stamps_open_transport_and_rejects_principal_body() -> None:
    client, core, _manager = _client()
    body = {
        **_caller(),
        "activation_id": "activation.1",
        "expected_record_revision": 2,
        "reason": None,
        "confirmed": True,
    }
    with client:
        response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json=body,
        )
        assert response.status_code == 200
        assert core.acknowledgements[-1]["transport_kind"] == "http"
        assert core.acknowledgements[-1]["assurance"] == "unverified"
        assert core.acknowledgements[-1]["trusted_principal_id"] is None

        response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json={**body, "trusted_principal_id": "forged"},
        )
        assert response.status_code == 422


def test_configured_auth_adapter_alone_can_stamp_trusted_principal() -> None:
    client, core, _manager = _client(trusted=True)
    with client:
        response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json={
                **_caller(),
                "activation_id": "activation.1",
                "expected_record_revision": 2,
                "reason": None,
                "confirmed": True,
            },
        )
    assert response.status_code == 200
    assert core.acknowledgements[-1]["trusted_principal_id"] == "operator.7"
    assert core.acknowledgements[-1]["assurance"] == "authenticated:test"


def test_bulk_acknowledgement_uses_one_common_owner_epoch_fence() -> None:
    client, core, _manager = _client()
    with client:
        response = client.post(
            "/api/execution/alarms/acknowledge-all",
            json={
                **_caller(),
                "members": [],
                "expected_generation": 3,
                "reason": None,
                "confirmed": True,
            },
        )
    assert response.status_code == 200
    mutation = core.acknowledgements[-1]
    assert mutation["request_owner_epoch"] == "client.epoch.1"
    assert mutation["expected_generation"] == 3
    assert "expected_owner_epoch" not in mutation


def test_alarm_acknowledgements_wake_the_control_projection(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    wakes: list[None] = []
    monkeypatch.setattr(
        HeadlessControlStateSource,
        "notify_cold",
        lambda _source: wakes.append(None),
    )
    client, _core, _manager = _client()
    exact = {
        **_caller(),
        "activation_id": "activation.1",
        "expected_record_revision": 2,
        "reason": None,
        "confirmed": True,
    }
    bulk = {
        **_caller(),
        "members": [],
        "expected_generation": 3,
        "reason": None,
        "confirmed": True,
    }
    with client:
        wake_count = len(wakes)
        exact_response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json=exact,
        )
        bulk_response = client.post(
            "/api/execution/alarms/acknowledge-all",
            json=bulk,
        )

    assert exact_response.status_code == 200
    assert bulk_response.status_code == 200
    assert len(wakes) == wake_count + 2


def test_historian_reconciliation_is_an_attributed_public_mutation() -> None:
    client, core, _manager = _client(trusted=True)
    with client:
        response = client.post(
            "/api/execution/history/reconcile",
            json={**_caller(), "expected_health_generation": 1},
        )
    assert response.status_code == 200
    assert response.json()["reconciled"] is True
    mutation = core.historian_reconciliations[-1]
    assert mutation == {
        "expected_generation": 1,
        "request_id": "request.1",
        "request_owner_epoch": "client.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": "correlation.1",
        "transport_kind": "http",
        "adapter_id": "manyforge.headless.http.v1",
        "assurance": "authenticated:test",
        "trusted_principal_id": "operator.7",
    }


def test_apply_is_forwarded_and_remains_typed_unavailable_before_i6() -> None:
    client, _core, manager = _client()
    with client:
        response = client.post(
            "/api/manager/apply",
            json={
                **_caller(),
                "deployment_artifact_id": "deployment.1",
                "deployment_revision": "sha256:" + "b" * 64,
                "expected_current": {
                    "graph_revision": "graph.1",
                    "retained_bundle_id": "bundle.1",
                },
                "reject_if_affected_busy": True,
                "prepared_activation": {
                    "schema_version": "manyforge.prepared_activation_input.v5",
                    "scenario_id": "scenario.1",
                    "source_document_digest": "sha256:" + "1" * 64,
                    "captured_input_digest": "sha256:" + "d" * 64,
                    "deployment_artifact_id": "deployment.1",
                    "deployment_revision": "sha256:" + "b" * 64,
                    "retained_bundle_id": "sha256:" + "2" * 64,
                    "graph_revision": "sha256:" + "3" * 64,
                    "program_id": "program.1",
                    "program_revision": "sha256:" + "4" * 64,
                    "program_outcome": {"kind": "bound", "program_binding_id": "sha256:" + "9" * 64},
                    "node_environment_digest": "sha256:" + "5" * 64,
                    "assistant_mode": "optional",
                    "prepared_activation_id": "sha256:" + "e" * 64,
                },
                "expected_admission_generation": 7,
                "candidate_receipt": {
                    "schema_version": "manyforge.manager_deployment_candidate_receipt.v2",
                    "deployment_artifact_id": "deployment.1",
                    "deployment_revision": "sha256:" + "b" * 64,
                    "prepared_activation_id": "sha256:" + "e" * 64,
                    "candidate_retained_bundle_id": "bundle.2",
                    "candidate_graph_revision": "graph.2",
                    "expected_current": {
                        "graph_revision": "graph.1",
                        "retained_bundle_id": "bundle.1",
                    },
                    "expected_admission_generation": 7,
                    "manager_owner_epoch": "client.epoch.1",
                    "impact_digest": "sha256:" + "c" * 64,
                    "destructive_impact": False,
                    "receipt_digest": "sha256:" + "d" * 64,
                },
            },
        )
    assert response.status_code == 503
    assert response.json()["error"]["code"] == "kernel_owner_unavailable"
    forwarded = manager.requests[0]
    assert forwarded.source_label == "operator_ui"
    assert forwarded.prepared_activation.prepared_activation_id == "sha256:" + "e" * 64


def test_apply_status_is_a_read_only_durable_identity_observation() -> None:
    client, _core, manager = _client()
    with client:
        response = client.get(
            "/api/manager/apply/status",
            params={"apply_id": "apply/7", "apply_generation": 7},
        )

    assert response.status_code == 200
    body = response.json()
    assert body["state"] == "pending"
    # One bounded lifecycle: the phase and the last real progress agree, and
    # neither is derived from responsiveness or a heartbeat.
    assert body["phase"] == body["last_progress"]["phase"] == "accepted"
    assert body["request_id"] == "request.1"
    assert body["prepared_activation"]["program_outcome"] == {
        "kind": "bound",
        "program_binding_id": "sha256:" + "9" * 64,
    }
    assert manager.apply_observations == [("apply/7", 7)]


class _CommittingManager(FakeManager):
    def __init__(self, state: str) -> None:
        super().__init__()
        self.state = state

    def observe_apply(self, apply_id: str, apply_generation: int) -> dict[str, Any]:
        return {**super().observe_apply(apply_id, apply_generation), "state": self.state}


class _NotifiedRunner:
    def __init__(self, error: ProgramRunnerClientError | None = None) -> None:
        self.error = error
        self.reconciliations = 0
        self.observed_activations: list[Any] = []
        self.core: FakeCore | None = None
        self.woken = threading.Condition()

    def reconcile_persistent_values(self) -> dict[str, Any]:
        with self.woken:
            self.reconciliations += 1
            if self.core is not None:
                # What a real runner reads on wake: the committed activation.
                self.observed_activations.append(
                    getattr(self.core, "committed_activation", None)
                )
            self.woken.notify_all()
        if self.error is not None:
            raise self.error
        return {"outcome": "reconciled"}

    def wait_for(self, count: int, timeout: float = 5.0) -> bool:
        with self.woken:
            return self.woken.wait_for(
                lambda: self.reconciliations >= count, timeout
            )

    def close(self) -> None:
        return None


def _status_reading_service(
    core: FakeCore, manager: FakeManager, runner: _NotifiedRunner
) -> HeadlessService:
    service = _service(core, manager, program_runner=runner)
    assert service.apply_follower is not None
    service.apply_follower._poll = 0.001  # noqa: SLF001
    return service


@pytest.mark.parametrize(
    ("state", "notified"),
    [("succeeded", 1), ("pending", 0), ("failed", 0), ("unknown_effect", 0)],
)
def test_only_an_observed_committed_apply_notifies_the_program_runner(
    state: str, notified: int
) -> None:
    runner = _NotifiedRunner()
    manager = _CommittingManager(state)
    core = FakeCore()
    core.committed_activation = _committed_activation("apply.1")  # type: ignore[attr-defined]
    service = _status_reading_service(core, manager, runner)
    with TestClient(create_app(service=service)) as client:
        response = client.get(
            "/api/manager/apply/status",
            params={"apply_id": "apply.1", "apply_generation": 1},
        )
        assert response.status_code == 200
        assert response.json()["state"] == state
        if notified:
            assert runner.wait_for(notified)
        else:
            # Only a committed Apply is followed at all.
            assert not service.apply_follower._followed  # noqa: SLF001
    assert runner.reconciliations == notified


def test_a_status_read_wakes_the_runner_only_once_manager_source_names_it() -> None:
    """Manager answers status from its journal and publishes its source just
    after: a runner woken in between would read a refusal (the live symptom
    program_applied_selection_not_active / program_activation_history_incomplete).
    The wake waits until the committed activation names the Apply."""

    runner = _NotifiedRunner()
    manager = _CommittingManager("succeeded")
    core = FakeCore()
    runner.core = core
    projected = Event()
    lagging_reads = 0

    def lagging_activation() -> dict[str, Any]:
        nonlocal lagging_reads
        if not projected.is_set():
            lagging_reads += 1
            raise HeadlessApiError(
                "program_applied_selection_not_active",
                "Applied Program requires one converged active configuration",
            )
        return _committed_activation("apply.1")

    core.committed_scenario_activation = lagging_activation  # type: ignore[method-assign]
    service = _status_reading_service(core, manager, runner)
    with TestClient(create_app(service=service)) as client:
        response = client.get(
            "/api/manager/apply/status",
            params={"apply_id": "apply.1", "apply_generation": 1},
        )
        assert response.json()["state"] == "succeeded"
        deadline = time.monotonic() + 5
        while lagging_reads < 3 and time.monotonic() < deadline:
            time.sleep(0.001)
        assert lagging_reads >= 3
        assert runner.reconciliations == 0  # never woken into the refusal
        core.committed_activation = _committed_activation("apply.1")  # type: ignore[attr-defined]
        projected.set()
        assert runner.wait_for(1)
    assert runner.reconciliations == 1
    assert runner.observed_activations == [_committed_activation("apply.1")]


def test_a_failed_cleanup_never_changes_the_committed_apply_outcome() -> None:
    runner = _NotifiedRunner(
        ProgramRunnerClientError(
            "persistent_key_storage_unavailable", "read-only file system", 503
        )
    )
    manager = _CommittingManager("succeeded")
    core = FakeCore()
    core.committed_activation = _committed_activation("apply.1")  # type: ignore[attr-defined]
    service = _status_reading_service(core, manager, runner)
    observed = []
    with TestClient(create_app(service=service)) as client:
        for count in (1, 2):
            observed.append(
                client.get(
                    "/api/manager/apply/status",
                    params={"apply_id": "apply.1", "apply_generation": 1},
                )
            )
            assert runner.wait_for(count)
            # The finished follow is released, so a later read follows again.
            deadline = time.monotonic() + 5
            while service.apply_follower._followed and time.monotonic() < deadline:  # noqa: SLF001
                time.sleep(0.001)
    # The Apply stays succeeded, unchanged, however often it is observed; the
    # storage fault is the runner's to report.
    assert [response.status_code for response in observed] == [200, 200]
    assert observed[0].json() == observed[1].json()
    assert observed[0].json()["state"] == "succeeded"
    assert runner.reconciliations == 2


def _committed_activation(apply_id: str = "apply.2") -> dict[str, Any]:
    return {
        "schema_version": "manyforge.committed_scenario_activation.v1",
        "apply_id": apply_id,
        "apply_generation": 1,
        "publication_generation": 3,
        "record_digest": "sha256:" + "d" * 64,
        "scenario_id": "scenario.1",
        "program": None,
        "committed_applies": [
            {
                "apply_id": "apply.1",
                "apply_generation": 1,
                "record_digest": "sha256:" + "c" * 64,
                "scenario_id": "scenario.2",
            },
            {
                "apply_id": apply_id,
                "apply_generation": 1,
                "record_digest": "sha256:" + "d" * 64,
                "scenario_id": "scenario.1",
            },
        ],
    }


def test_committed_activation_projects_every_committed_apply_of_the_history() -> None:
    core = FakeCore()
    core.committed_activation = _committed_activation()  # type: ignore[attr-defined]
    with TestClient(create_app(service=_service(core, FakeManager()))) as client:
        response = client.get("/api/program/committed-activation")
        assert response.status_code == 200
        assert response.json() == _committed_activation()

        # A history that does not account for every publication is refused.
        core.committed_activation = {  # type: ignore[attr-defined]
            **_committed_activation(),
            "publication_generation": 4,
        }
        refused = client.get("/api/program/committed-activation")
        assert refused.json()["detail"]["code"] == "program_activation_invalid"

        # Every committed Apply is named by its Apply identity; the current
        # one by the same identity it is current under.
        anonymous = _committed_activation()
        del anonymous["committed_applies"][0]["apply_id"]
        renamed = _committed_activation()
        renamed["committed_applies"][1]["apply_generation"] = 2
        for invalid in (anonymous, renamed):
            core.committed_activation = invalid  # type: ignore[attr-defined]
            refused = client.get("/api/program/committed-activation")
            assert refused.json()["detail"]["code"] == "program_activation_invalid"


def _scene_body() -> dict[str, Any]:
    scene_id = "sha256:" + "d" * 64
    return {
        "request_context": {
            **_caller(),
            "request_owner_epoch": scene_id,
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
        "expected_admission_generation": _ADMISSION_GENERATION,
        "operation": {
            "kind": "remove_object",
            "scene_resource_id": "part.1",
        },
    }


def test_scene_transaction_is_forwarded_with_adapter_only_attribution() -> None:
    client, _core, manager = _client(trusted=True)
    with client:
        response = client.post("/api/scene/transactions", json=_scene_body())
        forged = client.post(
            "/api/scene/transactions",
            json={
                **_scene_body(),
                "attribution": {
                    "transport": "http",
                    "assurance": "externally_authenticated",
                    "principal_id": "forged",
                },
            },
        )
    assert response.status_code == 200
    assert response.json()["projection_status"] == "committed"
    assert manager.scene_requests[0][0].operation.scene_resource_id == "part.1"
    assert manager.scene_requests[0][1] == "operator.7"
    assert forged.status_code == 422


def test_scene_result_lookup_never_submits_the_transaction(monkeypatch) -> None:
    client, _core, manager = _client()
    requests = []

    def lookup(request):
        requests.append(request)
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "ok": True,
            "result": None,
        }

    monkeypatch.setattr(manager, "scene_transaction_status", lookup, raising=False)
    with client:
        response = client.post("/api/scene/transactions/status", json=_scene_body())
    assert response.status_code == 200
    assert response.json()["result"] is None
    assert (
        requests[0].request_context.request_id
        == _scene_body()["request_context"]["request_id"]
    )
    assert manager.scene_requests == []


def test_scene_pose_query_is_read_only_revision_fenced_and_canonical() -> None:
    client, _core, manager = _client()
    projection = _scene_body()["expected_projection_revision"]
    body = {
        "api_version": "manyforge.scene_resource_pose_query.v1",
        "expected_projection_revision": projection,
        "scene_resource_ids": ["part.1", "part.2"],
    }
    with client:
        response = client.post("/api/scene/resource-poses/query", json=body)
        noncanonical = client.post(
            "/api/scene/resource-poses/query",
            json={**body, "scene_resource_ids": ["part.2", "part.1"]},
        )

    assert response.status_code == 200
    assert [row["scene_resource_id"] for row in response.json()["poses"]] == [
        "part.1",
        "part.2",
    ]
    assert manager.scene_pose_requests[0].expected_projection_revision.model_dump() == (
        projection
    )
    assert noncanonical.status_code == 422


def test_scene_pose_error_preserves_structured_owner_details() -> None:
    client, _core, manager = _client()
    projection = _scene_body()["expected_projection_revision"]

    def reject(_request: SceneResourcePoseQuery) -> dict[str, Any]:
        raise HeadlessApiError(
            "missing_reference",
            "active native projections disagree",
            details={"reason": "scene_frame_resolution_ambiguous"},
        )

    manager.resolve_scene_resources = reject  # type: ignore[method-assign]
    with client:
        response = client.post(
            "/api/scene/resource-poses/query",
            json={
                "api_version": "manyforge.scene_resource_pose_query.v1",
                "expected_projection_revision": projection,
                "scene_resource_ids": ["part.1"],
            },
        )

    assert response.status_code == 404
    assert response.json()["detail"]["details"] == {
        "reason": "scene_frame_resolution_ambiguous"
    }


def test_scene_support_declaration_crosses_http_without_native_names() -> None:
    client, _core, manager = _client(trusted=True)
    body = _scene_body()
    body["operation"] = {
        "kind": "add_object",
        "scene_resource_id": "part.1",
        "shape": {
            "type": "box",
            "box_dimensions_m": {"x_m": 0.1, "y_m": 0.2, "z_m": 0.3},
        },
        "pose": {
            "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.15},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        },
        "frame_ref": {"kind": "cell_frame", "cell_id": "cell-a"},
        "support_contacts": [
            {
                "support_object_id": "table.1",
                "mode": "bounded_penetration",
                "max_contact_penetration_m": 0.002,
            }
        ],
    }
    with client:
        response = client.post("/api/scene/transactions", json=body)
        openapi = client.get("/openapi.json").json()

    assert response.status_code == 200
    operation = manager.scene_requests[0][0].operation
    assert operation.support_contacts[0].support_object_id == "table.1"
    assert operation.support_contacts[0].max_contact_penetration_m == 0.002
    assert (
        "support_contacts"
        in openapi["components"]["schemas"]["AddSceneObjectRequest"]["properties"]
    )
    assert (
        "body_a"
        not in openapi["components"]["schemas"]["SceneSupportContactRequest"][
            "properties"
        ]
    )


def test_request_body_bound_is_enforced_before_model_decode() -> None:
    client, _core, _manager = _client()
    with client:
        response = client.post(
            "/api/manager/apply",
            content=b"x" * (1024 * 1024 + 1),
            headers={"content-type": "application/json"},
        )
    assert response.status_code == 413, response.text
    assert response.json()["detail"]["code"] == "headless_request_body_oversized"


def test_streamed_request_body_bound_is_enforced_without_content_length() -> None:
    client, _core, _manager = _client()

    def oversized_chunks():
        yield b"x" * (512 * 1024)
        yield b"y" * (512 * 1024)
        yield b"z"

    with client:
        response = client.post(
            "/api/manager/apply",
            content=oversized_chunks(),
            headers={"content-type": "application/json"},
        )
    assert response.status_code == 413, response.text
    assert response.json()["detail"]["code"] == "headless_request_body_oversized"


def test_recognized_malformed_stop_promotes_instead_of_returning_validation_error() -> (
    None
):
    execution = FakeExecution()
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    with TestClient(app) as client:
        response = client.post(
            "/api/execution/stops",
            content=b'{"request_context":',
            headers={"content-type": "application/json", "x-request-id": "http.stop.1"},
        )
        oversized = client.post(
            "/api/execution/stops",
            content=b"x" * (1024 * 1024 + 1),
            headers={"content-type": "application/json", "x-request-id": "http.stop.2"},
        )
        malformed_full = client.post(
            "/api/execution/full-stop",
            json={},
            headers={"x-request-id": "http.stop.3"},
        )

    assert response.status_code == 200, response.text
    assert oversized.status_code == 200, oversized.text
    assert malformed_full.status_code == 200, malformed_full.text
    assert oversized.headers["connection"] == "close"
    assert execution.requests == [
        (
            "recognized_malformed_stop",
            {
                "request_id": "malformed-stop.http.stop.1",
                "recognized_route": "/api/execution/stops",
                "reason": "recognized_stop_payload_invalid",
            },
        ),
        (
            "recognized_malformed_stop",
            {
                "request_id": "malformed-stop.http.stop.2",
                "recognized_route": "/api/execution/stops",
                "reason": "recognized_stop_body_oversized",
            },
        ),
        (
            "recognized_malformed_stop",
            {
                "request_id": "malformed-stop.http.stop.3",
                "recognized_route": "/api/execution/full-stop",
                "reason": "recognized_stop_payload_invalid",
            },
        ),
    ]


def test_query_cardinality_and_safe_integer_bounds_precede_native_conversion() -> None:
    client, _core, _manager = _client()
    with client:
        oversized_integer = client.get(
            "/api/execution/history",
            params={"before_sequence": 9_007_199_254_740_992},
        )
        excessive_states = client.get(
            "/api/execution/alarms",
            params=[("states", "active_unacknowledged")] * 5,
        )
        invalid_ordering = client.get(
            "/api/execution/alarms",
            params={"ordering": "priority_oldest"},
        )
        excessive_alarm_page = client.get(
            "/api/execution/alarms",
            params={"limit": 129},
        )
        excessive_history_page = client.get(
            "/api/execution/history",
            params={"limit": 33},
        )
    assert oversized_integer.status_code == 422
    assert excessive_states.status_code == 422
    assert invalid_ordering.status_code == 422
    assert excessive_alarm_page.status_code == 422
    assert excessive_history_page.status_code == 422


def test_history_detail_is_an_exact_bounded_read() -> None:
    client, _core, _manager = _client()
    with client:
        response = client.get("/api/execution/history/7")
        invalid = client.get("/api/execution/history/0")

    assert response.status_code == 200, response.text
    assert response.json()["sequence"] == 7
    assert set(response.json()["sections"]) == {
        "blackboard",
        "execution_digest",
        "incident",
        "latency",
    }
    assert invalid.status_code == 422


def test_skills_routes_use_exact_selected_environment_and_never_execute():
    client, core, manager = _client()
    with client:
        discovery = client.get("/api/skills")
        assert discovery.status_code == 200, discovery.text
        offered = discovery.json()
        assert offered["schema_version"] == "manyforge.skill_catalog.v1"
        assert offered["skills"][0]["body"]["node_id"] == "motion.move_to_joint_target"
        body = {
            "schema_version": "manyforge.skill_expansion_request.v1",
            "expected_environment_digest": offered["environment_digest"],
            "skill_id": "move_to_named_config",
            "instance_prefix": "skills.home",
            "arguments": {
                "configuration_name": "home",
                "goal": [{"joint_name": "joint1", "position_rad": 0.0}],
                "velocity_scale": 0.2,
                "acceleration_scale": 0.3,
                "max_tcp_linear_speed_mps": 0.1,
            },
            "component_roles": {"robot": "robot"},
            "target_roles": {"arm": "arm"},
            "program_context": {
                key: value
                for key, value in Program(
                    _ROOT / "examples/lite6_lite_gripper_pick_and_place.program.yaml",
                    build_node_environment().catalog,
                )
                .to_dict()
                .items()
                if key not in {"tree", "program_revision"}
            },
        }
        result = client.post("/api/skills/expand", json=body)
        assert result.status_code == 200, result.text
        assert result.json()["tree"]["instance_id"] == "skills.home.move"
        assert result.json()["environment_digest"] == offered["environment_digest"]
        assert (
            client.post(
                "/api/skills/expand", json={**body, "python_import": "os"}
            ).status_code
            == 422
        )
        assert (
            client.post(
                "/api/skills/expand",
                json={**body, "expected_environment_digest": "sha256:" + "0" * 64},
            ).status_code
            == 409
        )
        invalid = {**body, "target_roles": {"arm": "gripper"}}
        assert client.post("/api/skills/expand", json=invalid).status_code == 400
        selected = core.node_environment_projection()
        selected["environment_digest"] = "sha256:" + "0" * 64
        core.node_environment_projection = lambda: selected
        assert client.get("/api/skills").status_code == 503
        assert manager.requests == []
        assert manager.scene_requests == []


def test_program_diagnostic_route_is_bounded_attributed_evidence_only():
    client, core, _manager = _client()
    records = []

    def record(value):
        records.append(value)
        return {
            "schema_version": "manyforge.program_diagnostic_record.v1",
            "recorded": True,
        }

    core.record_program_diagnostic = record
    payload = {
        "schema_version": "manyforge.program_diagnostic.v3",
        "program_run_id": "run.1",
        "owner_epoch": "runner.1",
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "b" * 64,
        "retained_bundle_id": "sha256:" + "c" * 64,
        "graph_revision": "sha256:" + "d" * 64,
        "state": "failed",
        "reason_code": "value_mismatch",
        "message": "Check failed",
        "occurred_at_ns": "9007199254740993",
        "occurred_at_wall": "2026-09-08T10:00:00Z",
        "blackboard": {"ready": False},
        "omitted_blackboard_count": 0,
        "delivery_loss_count": 2,
        "details": {"input_problem": "type_mismatch", "path": "/ready"},
        "origin": {
            "kind": "program_node",
            "node_instance_id": "check.ready",
            "owner_api": None,
        },
    }
    with client:
        valid = client.post("/api/program/diagnostics", json=payload)
        assert valid.status_code == 200, valid.text
        assert records == [payload]
        for invalid in (
            {**payload, "schema_version": "manyforge.program_diagnostic.v1"},
            {
                **payload,
                "schema_version": "manyforge.program_diagnostic.v2",
                "runtime_overrides": {},
                "omitted_override_count": 0,
            },
            {key: value for key, value in payload.items() if key != "origin"},
            {**payload, "origin": None},
            {**payload, "state": "succeeded"},
            {**payload, "details": {"unregistered": "x"}},
            {**payload, "details": {}},
            {**payload, "origin": {**payload["origin"], "owner_api": "scene.frames"}},
            {**payload, "accepted": True},
            {**payload, "occurred_at_ns": str(2**63)},
            {**payload, "occurred_at_ns": "01"},
            {**payload, "blackboard": {str(i): i for i in range(33)}},
            {**payload, "blackboard": {"large": "x" * 3000}},
            {**payload, "blackboard": {"wide": [0] * 513}},
            {**payload, "blackboard": {"huge": "x" * 65536}},
        ):
            response = client.post("/api/program/diagnostics", json=invalid)
            assert response.status_code in (413, 422), response.text
        assert records == [payload]


def test_capture_page_is_a_bounded_validated_read() -> None:
    client, core, _manager = _client()
    calls: list[dict[str, Any]] = []

    def history_captures(**kwargs: Any) -> dict[str, Any]:
        calls.append(kwargs)
        return {
            "schema_version": "manyforge.historian_capture_page.v1",
            "captures": [],
            "truncated": False,
            "next_before_capture_id": None,
            "retention": {},
        }

    core.history_captures = history_captures  # type: ignore[attr-defined]
    with client:
        response = client.get("/api/execution/history/captures?before_capture_id=7&limit=5")
        assert response.status_code == 200, response.text
        assert calls == [{"before_capture_id": 7, "limit": 5}]
        assert client.get("/api/execution/history/captures?limit=0").status_code == 422
        core.history_captures = lambda **_kwargs: {  # type: ignore[attr-defined]
            "schema_version": "manyforge.historian_capture_page.v1",
            "captures": [{}] * 6,
            "truncated": False,
        }
        invalid = client.get("/api/execution/history/captures?limit=5")
    # Same mapping as every historian page read: the typed code, not 200.
    assert invalid.status_code == 400
    assert "historian_response_invalid" in invalid.text


def test_history_list_is_summary_only_and_keeps_exact_detail_on_demand() -> None:
    client, core, _manager = _client()
    large_payload = {
        "message": "Control connection stale",
        "program_run_id": "program-run.7",
        "nested": {
            "operation_id": "operation.9",
            "diagnostic_blob": "x" * 32_000,
        },
    }

    def history(**_kwargs: Any) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.historian_summary_page.v2",
            "events": [
                {
                    "sequence": 7,
                    "event": {
                        "schema_version": "manyforge.historian_event_summary.v2",
                        "event_id": "event-7",
                        "component": "manyforge.composer",
                        "event_type": "client_incident_activated",
                        "severity": "ERROR",
                        "occurred_at_ns": "9007199254740992",
                        "occurred_at_wall": "2026-09-04T10:00:00Z",
                        "occurred_at_utc_ns": "1788516000000000000",
                        "clock_owner": "headless:owner.1",
                        "wall_clock_regressed": False,
                        "condition_severity": "ERROR",
                        "message": "Control connection stale",
                        "program_run_id": "program-run.7",
                        "operation_id": "operation.9",
                        "condition_id": None,
                    },
                }
            ],
            "next_before_sequence": 7,
            "truncated": True,
        }

    def history_detail(sequence: int) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.historian_detail.v2",
            "sequence": sequence,
            "event": {
                "schema_version": "manyforge.historian_event.v2",
                "event_id": "event-7",
                "component": "manyforge.composer",
                "event_type": "client_incident_activated",
                "severity": "ERROR",
                "occurred_at_ns": "9007199254740992",
                "occurred_at_wall": "2026-09-04T10:00:00Z",
                "payload": large_payload,
            },
            "sections": {
                name: {"available": False, "reason": "not published", "value": None}
                for name in ("incident", "execution_digest", "blackboard", "latency")
            },
        }

    core.history = history  # type: ignore[method-assign]
    core.history_detail = history_detail  # type: ignore[method-assign]
    with client:
        response = client.get("/api/execution/history?limit=32")
        detail = client.get("/api/execution/history/7")

    assert response.status_code == 200, response.text
    page = response.json()
    assert page["schema_version"] == "manyforge.historian_summary_page.v2"
    assert len(response.content) < 2_048
    assert "payload" not in page["events"][0]["event"]
    assert page["events"][0]["event"] == {
        "schema_version": "manyforge.historian_event_summary.v2",
        "event_id": "event-7",
        "component": "manyforge.composer",
        "event_type": "client_incident_activated",
        "severity": "ERROR",
        "occurred_at_ns": "9007199254740992",
        "occurred_at_wall": "2026-09-04T10:00:00Z",
        "occurred_at_utc_ns": "1788516000000000000",
        "clock_owner": "headless:owner.1",
        "wall_clock_regressed": False,
        "condition_severity": "ERROR",
        "message": "Control connection stale",
        "program_run_id": "program-run.7",
        "operation_id": "operation.9",
        "condition_id": None,
    }
    assert detail.status_code == 200
    assert detail.json()["event"]["payload"] == large_payload


def test_openapi_contains_no_condition_mutation_route() -> None:
    client, _core, _manager = _client()
    with client:
        paths = client.get("/openapi.json").json()["paths"]
    assert "/api/execution/alarms/{condition_id}/acknowledge" in paths
    assert "/api/node-environment" in paths
    assert "/api/source-model" in paths
    assert "/api/source-model-context" in paths
    assert "/api/scenarios" not in paths
    assert "/api/viewer-model/urdf" in paths
    assert "/api/viewer-model/resource" in paths
    assert "/api/assistant/modes" in paths
    assert "/api/assistant/modes/{mode_id}" in paths
    assert "/api/assistant/modes/{mode_id}/context" in paths
    assert {
        "/api/control/diagnostics",
        "/api/execution/diagnostics",
        "/api/execution/identity",
        "/api/execution/targets",
        "/api/execution/control-state",
    }.issubset(paths)
    assert "/api/execution/observation" not in paths
    # The runtime-override workflow is removed; runtime keys stay writable.
    assert not any("runtime-overrides" in path for path in paths)
    assert "/api/program/runs/{program_run_id}/blackboard-values" in paths
    assert {
        "/api/execution/operations/{operation_id}/pause",
        "/api/execution/operations/{operation_id}/resume",
        "/api/execution/operations/{operation_id}/reset",
        "/api/execution/operations/{operation_id}/acknowledge-completed",
        "/api/execution/runtime-scale-control",
        "/api/execution/motion-previews",
        "/api/execution/capability-command-previews",
        "/api/execution/stops",
        "/api/execution/full-stop",
        "/api/execution/stop-fault",
        "/api/execution/stop-fault/recover",
        "/api/execution/pick-place/plans/pick",
        "/api/execution/pick-place/plans/place",
    }.issubset(paths)
    assert all("pick-place/sequences" not in path for path in paths)
    assert "/api/kernel/stop" not in paths
    assert "/api/cycle/stop" not in paths
    assert all("condition" not in path or "acknowledge" in path for path in paths)


def test_execution_diagnostics_is_explicit_and_status_alias_is_retired() -> None:
    # H1a-IF.2 cutover: the deprecated motion-centred status alias is gone.
    execution = FakeExecution()
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    with TestClient(app) as client:
        diagnostics = client.get("/api/execution/diagnostics")
        retired = client.get("/api/execution/status")

    assert diagnostics.status_code == 200
    assert diagnostics.json() == {"ok": True, "command": "status", "status": {}}
    assert retired.status_code == 404


def test_control_diagnostics_are_bounded_and_do_not_start_source_work() -> None:
    client, _core, _manager = _client()
    with client:
        diagnostics = client.get("/api/control/diagnostics")

    assert diagnostics.status_code == 200
    value = diagnostics.json()
    assert value["schema_version"] == "manyforge.control_plane_diagnostics.v1"
    assert value["active_clients"] == 0
    assert value["owner_reads"] == 0
    assert value["reductions"] == 0
    assert value["messages_sent"] == {
        "server_hello": 0,
        "heartbeat_challenge": 0,
        "snapshot": 0,
        "delta": 0,
        "command_receipt": 0,
        "resync_required": 0,
    }


def test_execution_observation_websocket_proxies_one_persistent_credited_session() -> (
    None
):
    class ObservationSession:
        def __init__(self) -> None:
            self.cursors: list[dict[str, Any] | None] = []
            self.entered = 0
            self.closed = False

        def __enter__(self) -> "ObservationSession":
            self.entered += 1
            return self

        def next(
            self,
            cursor: dict[str, Any] | None,
            _cancelled: object,
        ) -> dict[str, Any]:
            self.cursors.append(copy.deepcopy(cursor))
            return {
                "api_version": "manyforge.execution_command_response.v1",
                "command": "observation",
                "observation": {
                    "schema_version": "manyforge.execution_observation.v3",
                    "source_sequence": len(self.cursors),
                },
                "ok": True,
            }

        def close(self) -> None:
            self.closed = True

    session = ObservationSession()
    execution = FakeExecution()
    execution.open_observation_session = lambda: session  # type: ignore[attr-defined]
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    first_credit = {
        "last_applied": None,
        "schema_version": "manyforge.execution_observation_credit.v1",
    }
    applied_cursor = {
        "owner_epoch": "owner-1",
        "source_epoch": "source-1",
        "source_sequence": 1,
    }

    with TestClient(app) as client:
        with client.websocket_connect("/ws/execution/observations") as socket:
            socket.send_json(first_credit)
            assert socket.receive_json()["observation"]["source_sequence"] == 1
            socket.send_json(
                {
                    "last_applied": applied_cursor,
                    "schema_version": ("manyforge.execution_observation_credit.v1"),
                }
            )
            assert socket.receive_json()["observation"]["source_sequence"] == 2

    assert session.entered == 1
    assert session.cursors == [None, applied_cursor]
    assert session.closed is True


@pytest.mark.parametrize(
    "credit",
    [
        {},
        {
            "last_applied": None,
            "schema_version": "manyforge.execution_observation_credit.v1",
            "unexpected": True,
        },
        {
            "last_applied": {
                "owner_epoch": "owner-1",
                "source_epoch": "source-1",
                "source_sequence": True,
            },
            "schema_version": "manyforge.execution_observation_credit.v1",
        },
    ],
)
def test_execution_observation_credit_parser_rejects_non_exact_messages(
    credit: dict[str, Any],
) -> None:
    with pytest.raises((RuntimeError, ValueError)):
        _execution_observation_cursor(json.dumps(credit))


def test_execution_observation_credit_parser_rejects_oversized_messages() -> None:
    with pytest.raises(ValueError, match="oversized"):
        _execution_observation_cursor(" " * 4_097)


@pytest.mark.parametrize(
    "path,command,held",
    [
        ("motion-previews", "preview_motion", False),
        ("manual-handover", "read_manual_handover", False),
        ("manual-handover", "read_manual_handover", True),
    ],
)
def test_motion_preview_is_a_thin_non_effectful_execution_route(
    path, command, held
) -> None:
    execution = FakeExecution()
    context = {
        "program_runner_owner_epoch": "runner.1",
        "program_run_id": "held.program",
        "claim_generation": 1,
        "selection_revision": 3,
    }
    if held:
        execution.manual_handover = {
            "context": context,
            "producer_node_indices": list(range(100_000)),
        }
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    body = {
        "request_context": {
            "request_id": "request.preview.1",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
        },
        "program_id": "manyforge.direct_control.motion",
        "program_revision": "sha256:" + "a" * 64,
        "program_binding_id": "sha256:" + "b" * 64,
        "program_run_id": "direct-control-run.1",
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
                "required_capability_id": "execution.trajectory.execute",
                "action_span": None,
                "payload": {
                    "kind": "motion",
                    "move_id": "robot-control-joint-preview",
                    "base_frame_role_id": "base",
                    "tcp_frame_role_id": "tcp",
                    "tolerances": {
                        "joint_position_tol_rad": 0.001,
                        "translation_tol_m": 0.001,
                        "rotation_tol_rad": 0.001,
                    },
                    "speed_profile": {
                        "velocity_scale": 0.5,
                        "acceleration_scale": 0.5,
                        "max_tcp_linear_speed_mps": None,
                    },
                    "timeouts": {
                        "planning_timeout_s": 2.0,
                        "execution_timeout_s": 10.0,
                    },
                    "cache_mode": "default",
                    "planning_profile": "first_feasible",
                    "target_mode": "absolute",
                    "target_reference": None,
                    "offset_reference": None,
                    "motion": {
                        "kind": "joint_goal",
                        "joint_names": ["joint1"],
                        "positions_rad": [0.1],
                    },
                    "contact_policy_id": None,
                    "per_joint_dynamics_limits": {},
                    "motion_interrupt_scopes": [],
                    "runtime_object_bindings": [],
                },
            }
        ],
        "coordination_policy": "independent",
    }

    with TestClient(app) as client:
        response = client.post(f"/api/execution/{path}", json=body)

    assert response.status_code == 200, response.text
    assert response.json()["command"] == command
    assert [kind for kind, _request in execution.requests] == [command]
    assert execution.principals == [(command, None)]
    if command == "read_manual_handover":
        assert response.json()["manual_handover"] == (context if held else None)
        assert "handover" not in response.json()
        assert len(response.content) < 1024


@pytest.mark.parametrize("case", ["held", "wrong_path", "wrong_parent", "not_allowed"])
def test_manual_cleanup_uses_shared_control_ingress_and_exact_runner(case) -> None:
    runner = FakeProgramRunner()
    if case == "not_allowed":
        original = runner.cycle_status

        def disabled():
            result = original()
            for command in result["allowed_commands"]:
                if command["command_kind"] == "program_manual_handover":
                    command.update(
                        allowed=False, disabled_reason="program_cycle_state_starting"
                    )
            return result

        runner.cycle_status = disabled
    execution = FakeExecution()
    app = create_app(
        service=_service(FakeCore(), FakeManager(), execution, program_runner=runner)
    )
    body = {
        **_caller(),
        "request_id": "cleanup.1",
        "program_run_id": "program-run.1",
        "expected_owner_epoch": "program-runner.epoch.1",
        "expected_run_revision": 1,
        "operation": {
            "request_context": {
                "request_id": "manual.1",
                "request_owner_epoch": "execution.epoch.1",
                "source_label": "operator_ui",
                "correlation_id": None,
            },
            "program_id": "manyforge.direct_control.gripper",
            "program_revision": "sha256:" + "a" * 64,
            "program_binding_id": "sha256:" + "b" * 64,
            "program_run_id": "manual.1",
            "expected_graph_revision": "sha256:" + "c" * 64,
            "expected_retained_bundle_id": "sha256:" + "d" * 64,
            "expected_scene_revision": None,
            "coordination_policy": "independent",
            "manual_handover": {
                "program_runner_owner_epoch": "program-runner.epoch.1",
                "program_run_id": (
                    "other" if case == "wrong_parent" else "program-run.1"
                ),
                "claim_generation": 1,
                "selection_revision": 3,
            },
            "participants": [
                {
                    "component_role_id": "gripper",
                    "target_role_id": "gripper",
                    "required_capability_id": "tool.gripper.command",
                    "action_span": None,
                    "payload": {
                        "kind": "capability_command",
                        "command_schema_id": "gripper.command_position",
                        "command_schema_revision": "sha256:" + "a" * 64,
                        "completion_timeout_s": 2.0,
                        "caller_options": [],
                        "command_value": {
                            "kind": "gripper_position",
                            "position": {"kind": "linear_m", "value_m": 0.0},
                            "max_effort": None,
                        },
                    },
                }
            ],
        },
    }
    target = "other" if case == "wrong_path" else "program-run.1"
    with TestClient(app) as client:
        response = client.post(f"/api/program/runs/{target}/manual-handover", json=body)
    assert execution.requests == []
    if case == "held":
        assert response.status_code == 200, response.text
        assert response.json()["state"] == "running"
        assert [kind for kind, _ in runner.calls] == ["manual_handover"]
        run_id, received = runner.calls[0][1]
        assert run_id == "program-run.1"
        assert received["operation"] == body["operation"]
        assert received["expected_run_revision"] == 1
        assert received["request_id"] == "cleanup.1"
    else:
        assert (
            response.status_code
            == {
                "wrong_path": 400,
                "wrong_parent": 422,
                "not_allowed": 409,
            }[case]
        ), response.text
        assert runner.calls == []


def test_capability_command_preview_is_a_thin_non_effectful_execution_route() -> None:
    execution = FakeExecution()
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    body = {
        "request_context": {
            "request_id": "request.gripper-preview.1",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "operator_ui",
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

    with TestClient(app) as client:
        response = client.post("/api/execution/capability-command-previews", json=body)
        forged = client.post(
            "/api/execution/capability-command-previews",
            json={**copy.deepcopy(body), "attribution": {"transport": "http"}},
        )

    assert response.status_code == 200, response.text
    assert response.json()["command"] == "preview_capability_command"
    assert forged.status_code == 422
    assert [kind for kind, _request in execution.requests] == [
        "preview_capability_command"
    ]
    assert execution.principals == [("preview_capability_command", None)]


def test_execution_missing_reference_is_a_public_not_found() -> None:
    class MissingExecution:
        def diagnostics(self) -> dict[str, Any]:
            raise AssertionError("not called")

        def submit(
            self,
            _request: object,
            *,
            trusted_principal_id: str | None,
        ) -> dict[str, Any]:
            raise AssertionError("not called")

        def read(self, _operation_id: str) -> dict[str, Any]:
            return {
                "api_version": "manyforge.execution_command_response.v1",
                "ok": False,
                "error": {
                    "code": "missing_reference",
                    "message": "operation is not resident",
                    "details": {"reason": "operation_not_resident"},
                },
            }

    app = create_app(service=_service(FakeCore(), FakeManager(), MissingExecution()))
    with TestClient(app) as client:
        response = client.get("/api/execution/operations/operation-retired")

    assert response.status_code == 404
    assert response.json()["error"]["code"] == "missing_reference"


def test_operation_controls_and_scale_control_are_thin_fenced_routes() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(
            FakeCore(),
            FakeManager(),
            execution,
            program_runner=FakeProgramRunner(active=False),
        )
    )
    context = {
        "request_id": "request.control.1",
        "request_owner_epoch": "execution.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }
    control = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "reason": "operator request",
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
        "scale": 0.4,
    }

    with TestClient(app) as client:
        for endpoint in ("pause", "resume", "reset", "acknowledge-completed"):
            response = client.post(
                f"/api/execution/operations/operation-1/{endpoint}",
                json=control,
            )
            assert response.status_code == 200, response.text
        assert client.post("/api/execution/stops", json=stop).status_code == 200
        assert (
            client.post(
                "/api/execution/runtime-scale-control", json=scale_control
            ).status_code
            == 200
        )
        assert (
            client.get(
                "/api/execution/runtime-scale-control",
                params=scale_control["selector"],
            ).status_code
            == 200
        )
        group_query = '[{"component_role_id":"robot-a","target_role_id":"arm"},{"component_role_id":"robot-b","target_role_id":"arm"}]'
        assert (
            client.get(
                "/api/execution/runtime-scale-control",
                params={
                    "program_binding_id": "binding-1",
                    "group_targets": group_query,
                },
            ).status_code
            == 200
        )
        assert (
            client.get(
                "/api/execution/runtime-scale-control",
                params={
                    "program_binding_id": "binding-1",
                    "group_targets": '[{"target_role_id":"arm","target_role_id":"arm"}]',
                },
            ).status_code
            == 422
        )
        assert (
            client.get(
                "/api/execution/runtime-scale-control",
                params={
                    "program_binding_id": "binding-1",
                    "group_targets": '[{"component_role_id":"robot","target_role_id":NaN}]',
                },
            ).status_code
            == 422
        )
        valid_group = (
            '[{"component_role_id":"robot-a","target_role_id":"arm"},'
            '{"component_role_id":"robot-b","target_role_id":"arm"}]'
        )
        assert (
            client.get(
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
                client.get(
                    "/api/execution/runtime-scale-control", params=params
                ).status_code
                == 422
            )
        assert (
            client.get(
                "/api/execution/runtime-scale-control",
                params={
                    "program_binding_id": "binding-1",
                    "group_targets": "[" * 2000 + "]" * 2000,
                },
            ).status_code
            == 422
        )

    assert [kind for kind, _request in execution.requests] == [
        "pause",
        "resume",
        "reset",
        "acknowledge_completed",
        "stop",
        "set_runtime_scale_control",
        "get_runtime_scale_control",
        "get_runtime_scale_control",
    ]
    assert execution.requests[-1][1].model_dump(mode="json") == {
        "program_binding_id": "binding-1",
        "group_targets": [
            {"component_role_id": "robot-a", "target_role_id": "arm"},
            {"component_role_id": "robot-b", "target_role_id": "arm"},
        ],
    }
    assert execution.principals == [
        ("pause", None),
        ("resume", None),
        ("reset", None),
        ("acknowledge_completed", None),
        ("stop", None),
        ("set_runtime_scale_control", None),
    ]


def test_full_stop_and_stop_fault_recovery_are_thin_owner_routes() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(
            FakeCore(),
            FakeManager(),
            execution,
            program_runner=FakeProgramRunner(active=False),
        )
    )
    context = {
        "request_id": "request.full-stop.1",
        "request_owner_epoch": "execution.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }
    recovery_context = {**context, "request_id": "request.recover-stop.1"}
    recovery = {
        "request_context": recovery_context,
        "expected_latch_id": "stop-fault.1",
        "expected_latch_generation": 1,
        "expected_recovery_assessment_revision": 2,
        "expected_stop_certificate_id": "stop-certificate.1",
        "expected_stop_certificate_revision": 1,
        "expected_cell_id": "cell.1",
        "expected_retained_bundle_id": "sha256:" + "1" * 64,
        "expected_graph_revision": "sha256:" + "2" * 64,
        "configuration_expectations": [
            {
                "control_domain_id": "domain.1",
                "active_control_configuration_id": "configuration.1",
                "active_control_configuration_generation": 3,
            }
        ],
        "reason": "fresh settlement verified",
    }

    with TestClient(app) as client:
        full = client.post(
            "/api/execution/full-stop",
            json={
                "request_context": context,
                "expected_stop_generation": 0,
            },
        )
        fault = client.get("/api/execution/stop-fault")
        recovered = client.post("/api/execution/stop-fault/recover", json=recovery)

    assert full.status_code == 200
    assert fault.status_code == 200
    assert fault.json()["stop_fault"] is None
    assert recovered.status_code == 200
    assert [kind for kind, _request in execution.requests] == [
        "full_stop",
        "stop_fault",
        "recover_stop_fault",
    ]
    assert execution.principals == [("full_stop", None)]


def test_execution_mutation_principal_can_only_come_from_trusted_adapter() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(
            FakeCore(),
            FakeManager(),
            execution,
            program_runner=FakeProgramRunner(active=False),
        ),
        trusted_principal_adapter=FakeTrustedAdapter(),
    )
    body = {
        "request_context": {
            "request_id": "request.control.trusted",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
        },
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "reason": None,
    }
    with TestClient(app) as client:
        forged = client.post(
            "/api/execution/operations/operation-1/pause",
            json={**body, "principal_id": "forged"},
        )
        forged_context = client.post(
            "/api/execution/operations/operation-1/pause",
            json={
                **body,
                "request_context": {
                    **body["request_context"],
                    "principal_id": "forged",
                },
            },
        )
        accepted = client.post("/api/execution/operations/operation-1/pause", json=body)

    assert forged.status_code == 422
    assert forged_context.status_code == 422
    assert accepted.status_code == 200
    assert execution.principals == [("pause", "operator.7")]


def test_operation_control_paths_accept_bounded_stop_and_promote_mixed_scope() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(
            FakeCore(),
            FakeManager(),
            execution,
            program_runner=FakeProgramRunner(active=False),
        )
    )
    context = {
        "request_id": "request.control.1",
        "request_owner_epoch": "execution.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }
    with TestClient(app) as client:
        mismatch = client.post(
            "/api/execution/operations/operation-1/pause",
            json={
                "request_context": context,
                "operation_id": "operation-2",
                "expected_operation_revision": 1,
                "reason": None,
            },
        )
        broad_stop = client.post(
            "/api/execution/stops",
            json={
                "request_context": context,
                "operations": [
                    {
                        "operation_id": operation_id,
                        "expected_operation_revision": 1,
                        "expected_stop_generation": 0,
                    }
                    for operation_id in ("operation-1", "operation-2")
                ],
                "reason": None,
            },
        )
        mixed_stop = client.post(
            "/api/execution/stops",
            json={
                "request_context": context,
                "operations": [
                    {
                        "operation_id": "operation-1",
                        "expected_operation_revision": 1,
                        "expected_stop_generation": 0,
                    }
                ],
                "components": [
                    {
                        "controllable_component_id": "robot-1",
                        "expected_graph_revision": "sha256:" + "1" * 64,
                        "expected_component_lifecycle_generation": 1,
                    }
                ],
                "reason": None,
            },
        )
        invalid_scale = client.post(
            "/api/execution/runtime-scale-control",
            json={
                "request_context": context,
                "selector": {
                    "program_binding_id": "binding-1",
                    "component_role_id": "robot",
                    "target_role_id": "arm",
                },
                "expected_control_generation": 0,
                "scale": 1.01,
            },
        )

    assert mismatch.status_code == 400
    assert mismatch.json()["detail"]["code"] == ("execution_operation_path_invalid")
    assert broad_stop.status_code == 200
    assert mixed_stop.status_code == 200
    assert invalid_scale.status_code == 422
    assert [kind for kind, _request in execution.requests] == [
        "stop",
        "recognized_malformed_stop",
    ]


@pytest.mark.parametrize(
    ("code", "expected_status"),
    [
        ("operator_alarm_acknowledgement_confirmation_required", 400),
        ("operator_alarm_acknowledgement_reason_required", 400),
        ("operator_alarm_acknowledge_all_confirmation_required", 400),
        ("operator_alarm_membership_mismatch", 409),
    ],
)
def test_alarm_owner_rejections_preserve_their_public_http_class(
    code: str,
    expected_status: int,
) -> None:
    core = FakeCore()

    def reject(**_kwargs: Any) -> dict[str, Any]:
        raise HeadlessApiError(code, "injected owner rejection")

    core.acknowledge_alarm = reject  # type: ignore[method-assign]
    app = create_app(service=_service(core, FakeManager()))
    with TestClient(app) as client:
        response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json={
                **_caller(),
                "activation_id": "activation.1",
                "expected_record_revision": 1,
                "reason": None,
                "confirmed": True,
            },
        )

    assert response.status_code == expected_status
    assert response.json()["detail"]["code"] == code


class _PersistentValuesRunner:
    def __init__(self) -> None:
        self.calls: list[tuple[str, Any]] = []

    def persistent_values(self, scenario_id: str) -> dict[str, Any]:
        self.calls.append(("read", scenario_id))
        return {"scenario_id": scenario_id, "keys": []}

    def persistent_value_action(
        self, scenario_id: str, action: str, body: dict[str, Any]
    ) -> dict[str, Any]:
        self.calls.append((action, copy.deepcopy(body)))
        if body.get("key") == "halted":
            raise ProgramRunnerClientError(
                "persistent_key_run_conflict",
                "Program run program-cycle.1 refused the edit",
                409,
            )
        return {"action": action}


def test_persistent_values_proxy_the_program_runner_with_its_refusals() -> None:
    runner = _PersistentValuesRunner()
    client = TestClient(
        create_app(service=_service(FakeCore(), FakeManager(), program_runner=runner))
    )
    base = "/api/program/scenarios/cell-pick/persistent-values"
    caller = {
        "request_id": "request.1",
        "request_owner_epoch": "composer.epoch.1",
        "source_label": "operator_ui",
        "scenario_id": "cell-pick",
    }
    edit = {
        **caller,
        "key": "layer",
        "expected_program_revision": "sha256:" + "a" * 64,
    }

    assert client.get(base).json() == {"scenario_id": "cell-pick", "keys": []}
    assert client.post(base, json={**edit, "value": 2}).json() == {"action": "set"}
    assert client.post(f"{base}/reset", json=edit).json() == {"action": "reset"}
    assert client.post(f"{base}/clear", json=caller).json() == {"action": "clear"}
    refused = client.post(
        base,
        json={
            **edit,
            "key": "halted",
            "value": 2,
            "program_run_id": "run.1",
            "expected_run_revision": 3,
        },
    )
    assert refused.status_code == 409
    assert refused.json()["detail"]["code"] == "persistent_key_run_conflict"
    mismatch = client.post(
        "/api/program/scenarios/other/persistent-values/clear", json=caller
    )
    assert mismatch.status_code == 400
    assert mismatch.json()["detail"]["code"] == "persistent_key_path_invalid"

    assert [name for name, _body in runner.calls] == [
        "read",
        "set",
        "reset",
        "clear",
        "set",
    ]
    assert runner.calls[1][1]["value"] == 2
