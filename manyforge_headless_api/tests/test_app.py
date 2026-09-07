from __future__ import annotations

import copy
from functools import lru_cache
import hashlib
import json
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
from manyforge_headless_api.service import HeadlessService


_ROOT = Path(__file__).resolve().parents[2]
_BINDING_FIXTURE = (
    _ROOT
    / "manyforge_deployment/resources/program_binding_lite6_native.fixture.json"
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
    binding["node_environment"]["environment_digest"] = (
        build_node_environment().manifest.environment_digest
    )
    binding["program_id"] = program["program_id"]
    binding["program_revision"] = program["program_revision"]
    binding["retained_bundle_id"] = retained_bundle_id
    binding["graph_revision"] = graph_revision
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    return binding


def _applied_projection(
    program: dict[str, Any],
    *,
    scenario_id: str = "cell-pick",
    scenario_catalog_revision: str = "sha256:" + "6" * 64,
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
        "baseline_program_binding_id": "sha256:" + "3" * 64,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": "sha256:" + "1" * 64,
        "graph_revision": binding["graph_revision"],
        "node_environment_digest": binding["node_environment"][
            "environment_digest"
        ],
        "preparation_digest": "sha256:" + "4" * 64,
        "program": copy.deepcopy(program),
        "program_binding": binding,
        "program_binding_id": binding["program_binding_id"],
        "program_id": program["program_id"],
        "program_revision": program["program_revision"],
        "retained_bundle_id": binding["retained_bundle_id"],
        "scenario_catalog_revision": scenario_catalog_revision,
        "scenario_id": scenario_id,
        "schema_version": "manyforge.applied_program_selection.v1",
        "source": {
            "record_digest": record_digest,
            "semantic_content_digest": "sha256:" + "7" * 64,
            "source_generation": 1,
            "source_owner_epoch": "program-owner.epoch.1",
            "source_owner_id": "program-owner.cell-a",
        },
        "typed_parameter_overrides": {},
    }


def _scenario_selection(
    *,
    scenario_id: str = "cell-pick",
) -> dict[str, Any]:
    program = _program_document()
    binding = _program_binding(program)
    return {
        "assistant_mode": "forbidden",
        "assistant_projection": None,
        "deployment_artifact_id": "cell_a",
        "deployment_revision": "sha256:" + "1" * 64,
        "graph_revision": binding["graph_revision"],
        "node_environment_digest": binding["node_environment"][
            "environment_digest"
        ],
        "owner_identities": [],
        "program": copy.deepcopy(program),
        "program_binding": binding,
        "program_binding_id": binding["program_binding_id"],
        "program_id": program["program_id"],
        "program_revision": program["program_revision"],
        "retained_bundle_id": binding["retained_bundle_id"],
        "scenario_catalog_revision": "sha256:" + "6" * 64,
        "scenario_id": scenario_id,
        "schema_version": "manyforge.scenario_program_selection.v2",
        "typed_parameter_overrides": {},
    }


def _remove_program_tree_with_valid_identity(value: dict[str, Any]) -> None:
    value["program"].pop("tree")
    value["program"]["program_revision"] = compute_content_identity(
        value["program"],
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v1",
    )
    value["program_revision"] = value["program"]["program_revision"]
    value["program_binding"]["program_revision"] = value["program_revision"]
    value["program_binding"]["program_binding_id"] = compute_content_identity(
        value["program_binding"],
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    value["program_binding_id"] = value["program_binding"]["program_binding_id"]


def _malform_binding_frame_with_valid_identity(value: dict[str, Any]) -> None:
    binding = value["program_binding"]
    binding["target_role_bindings"][0]["frame_role_bindings"][0][
        "unknown"
    ] = True
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    value["program_binding_id"] = binding["program_binding_id"]


def test_closed_core_invalid_request_maps_to_a_client_error() -> None:
    assert _error_status("invalid_request") == 400
    assert _error_status("program_apply_request_oversized") == 413
    assert _error_status("full_stack_restart_required") == 409


def test_full_native_applied_program_projection_is_accepted() -> None:
    core = FakeCore()
    projection = _applied_projection(_program_document())
    core.applied_program = copy.deepcopy(projection)
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/program/applied")

    assert response.status_code == 200
    assert response.json() == projection


def test_scenario_program_selection_rejects_rehashed_malformed_binding() -> None:
    core = FakeCore()
    selection = _scenario_selection()
    selection["program_binding"]["target_role_bindings"][0][
        "frame_role_bindings"
    ][0]["unknown"] = True
    selection["program_binding"]["program_binding_id"] = compute_content_identity(
        selection["program_binding"],
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    core.scenario_selection_override = selection
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/scenarios/cell-pick/program-selection")

    assert response.status_code == 502
    assert response.json()["detail"]["code"] == "scenario_program_selection_invalid"


def test_scenario_load_selection_uses_effect_free_package_projection() -> None:
    core = FakeCore()
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.get("/api/scenarios/cell-pick/load-selection")

    assert response.status_code == 200
    assert response.json() == _scenario_selection()


@pytest.mark.parametrize(
    "break_projection",
    [
        lambda value: value.__setitem__("unknown", True),
        lambda value: value.pop("program"),
        lambda value: value["program"].__setitem__("program_id", "mismatch"),
        lambda value: value["program"].__setitem__(
            "program_revision", "sha256:" + "f" * 64
        ),
        _remove_program_tree_with_valid_identity,
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
        (("scenario_catalog_revision",), "catalog.1"),
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
        self.program_target_bundle = "bundle.1"
        self.program_target_graph = "graph.1"
        self.model_attempts: list[dict[str, Any]] = []
        self.source_model_context_override: dict[str, Any] | None = None
        self.scenario_selection_override: dict[str, Any] | None = None
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
                "manager": _section(
                    "component-manager.cell-a", self.manager_payload
                ),
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

    def scenario_catalog_projection(self) -> dict[str, Any]:
        return {
            "api_version": "manyforge.scenario_catalog.v1",
            "scenario_catalog_revision": "sha256:" + "6" * 64,
            "scenarios": [
                {
                    "scenario_id": "cell-pick",
                    "deployment_artifact_id": "cell_a",
                    "deployment_revision": "sha256:" + "1" * 64,
                    "retained_bundle_id": "sha256:" + "2" * 64,
                    "program_binding_id": "sha256:" + "3" * 64,
                    "deployment_compatibility": {
                        "lane": "manyforge-only",
                        "provides": [],
                    },
                    "assistant_mode": "forbidden",
                    "typed_parameter_overrides": {},
                }
            ],
        }

    def scenario_program_selection(self, scenario_id: str) -> dict[str, Any]:
        if self.scenario_selection_override is not None:
            return copy.deepcopy(self.scenario_selection_override)
        return _scenario_selection(scenario_id=scenario_id)

    def scenario_load_selection(self, scenario_id: str) -> dict[str, Any]:
        if self.scenario_selection_override is not None:
            return copy.deepcopy(self.scenario_selection_override)
        return _scenario_selection(scenario_id=scenario_id)

    def prepare_program_apply(
        self,
        scenario_id: str,
        scenario_catalog_revision: str,
        program: dict[str, Any],
    ) -> dict[str, Any]:
        active = self.manager_payload["active"]
        if (
            active["retained_bundle_id"] != self.program_target_bundle
            or active["graph_revision"] != self.program_target_graph
        ):
            raise HeadlessApiError(
                "program_apply_deployment_not_active",
                "Program Apply target is not the active deployment",
            )
        self.program_preparations.append(copy.deepcopy(program))
        return {
            "schema_version": "manyforge.program_apply_preparation.v1",
            "scenario_id": scenario_id,
            "scenario_catalog_revision": scenario_catalog_revision,
            "deployment_artifact_id": "cell_a",
            "deployment_revision": "sha256:" + "1" * 64,
            "retained_bundle_id": self.program_target_bundle,
            "graph_revision": self.program_target_graph,
            "node_environment_digest": "sha256:" + "2" * 64,
            "baseline_program_binding_id": "sha256:" + "3" * 64,
            "program_id": program["program_id"],
            "program_revision": program["program_revision"],
            "program_binding_id": "sha256:" + "3" * 64,
            "assistant_mode": "forbidden",
            "typed_parameter_overrides": {},
            "preparation_digest": "sha256:" + "4" * 64,
        }

    def replay_program_apply(
        self,
        scenario_id: str,
        scenario_catalog_revision: str,
        program_id: str,
        program_revision: str,
        expected_applied_record_digest: str | None,
        request_owner_epoch: str,
        request_id: str,
    ) -> dict[str, Any] | None:
        retained = self.program_replays.get((request_owner_epoch, request_id))
        if retained is None:
            return None
        semantic = (
            scenario_id,
            scenario_catalog_revision,
            program_id,
            program_revision,
            expected_applied_record_digest,
        )
        if retained[0] != semantic:
            raise HeadlessApiError(
                "program_apply_request_conflict",
                "Program Apply request identity was reused",
            )
        return copy.deepcopy(retained[1])

    def commit_program_apply(
        self,
        preparation: dict[str, Any],
        expected_applied_record_digest: str | None,
        request_owner_epoch: str,
        request_id: str,
    ) -> dict[str, Any]:
        assert request_owner_epoch
        assert request_id
        self.program_commits.append(
            (copy.deepcopy(preparation), expected_applied_record_digest)
        )
        record_digest = "sha256:" + "5" * 64
        self.applied_program = _applied_projection(
            self.program_preparations[-1],
            scenario_id=preparation["scenario_id"],
            scenario_catalog_revision=preparation["scenario_catalog_revision"],
            record_digest=record_digest,
            retained_bundle_id=preparation["retained_bundle_id"],
            graph_revision=preparation["graph_revision"],
        )
        semantic = (
            preparation["scenario_id"],
            preparation["scenario_catalog_revision"],
            preparation["program_id"],
            preparation["program_revision"],
            expected_applied_record_digest,
        )
        self.program_replays[(request_owner_epoch, request_id)] = (
            semantic,
            {
                "preparation": copy.deepcopy(preparation),
                "applied": copy.deepcopy(self.applied_program),
            },
        )
        return copy.deepcopy(self.applied_program)

    def applied_program_selection(self) -> dict[str, Any]:
        if self.applied_program is None:
            raise HeadlessApiError(
                "program_applied_selection_not_found",
                "no Program has been authoritatively Applied",
            )
        return copy.deepcopy(self.applied_program)

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
            "ok": True,
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
            "projection_revision": (
                request.expected_projection_revision.model_dump()
            ),
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
            "observation": {"schema_version": "manyforge.execution_observation.v2"},
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
        self.principals.append(
            ("preview_capability_command", trusted_principal_id)
        )
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

    def start_pick(
        self,
        request: object,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(("start_pick", request))
        self.principals.append(("start_pick", trusted_principal_id))
        return {
            "ok": True,
            "command": "start_pick",
            "planning_result": {},
            "sequence_status": {},
        }

    def start_place(
        self,
        request: object,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(("start_place", request))
        self.principals.append(("start_place", trusted_principal_id))
        return {
            "ok": True,
            "command": "start_place",
            "planning_result": {},
            "sequence_status": {},
        }

    def read_pick_place_sequence(self, sequence_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "command": "read_pick_place_sequence",
            "sequence_status": {"sequence_id": sequence_id},
        }

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
        return self._control(
            "acknowledge_completed", request, trusted_principal_id
        )

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
        self.principals.append(
            ("recognized_malformed_stop", trusted_principal_id)
        )
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
        self.principals.append(
            ("set_runtime_scale_control", trusted_principal_id)
        )
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
        "schema_version": "manyforge.program_run_status.v2",
        "owner_id": "program-runner.cell-a",
        "owner_epoch": "program-runner.epoch.1",
        "run_revision": 1,
        "state": state,
        "capture": {
            "schema_version": "manyforge.program_run_capture.v1",
            "program_run_id": "program-run.1",
            "applied_record_digest": "sha256:" + "5" * 64,
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": "sha256:" + "6" * 64,
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
            "program_runtime_override_stage",
            "program_runtime_override_activate",
            "program_runtime_override_clear",
            "program_runtime_override_promote",
            "program_runtime_blackboard_write",
            "program_manual_result_stage",
            "operation_start",
            "operation_stop",
            "operation_pause",
            "operation_resume",
            "operation_reset",
            "operation_acknowledge_completed",
            "pick_sequence_start",
            "place_sequence_start",
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
            snapshot["capture"]["node_environment_digest"] = (
                self.node_environment_digest
            )
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
            "prepared_scene": {
                "schema_version": "manyforge.prepared_program_scene.v1"
            },
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
        core = {key: copy.deepcopy(value) for key, value in row.items() if key != "owner_port_name"}
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
            "content_digest": "sha256:" + hashlib.sha256(
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
            or self.cycle["cycle_generation"]
            != body["expected_cycle_generation"]
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
    deployment_compiler: object | None = None,
) -> HeadlessService:
    return HeadlessService(
        core,
        manager,
        execution=execution,  # type: ignore[arg-type]
        program_runner=program_runner,  # type: ignore[arg-type]
        cell_admission=FakeCellAdmission(),
        cclc=cclc,
        deployment_compiler=deployment_compiler,  # type: ignore[arg-type]
    )


def _client(*, trusted: bool = False) -> tuple[TestClient, FakeCore, FakeManager]:
    core = FakeCore()
    manager = FakeManager()
    app = create_app(
        service=_service(core, manager),
        trusted_principal_adapter=FakeTrustedAdapter() if trusted else None,
    )
    return TestClient(app), core, manager


class FakeDeploymentCompiler:
    def __init__(self) -> None:
        self.calls: list[dict[str, Any]] = []

    def compile(self, **kwargs: Any) -> dict[str, Any]:
        self.calls.append(copy.deepcopy(kwargs))
        return {
            "schema_version": "manyforge.deployment_candidate_compilation.v1",
            "scenario_id": kwargs["scenario_id"],
            "scenario_catalog_revision": kwargs[
                "expected_scenario_catalog_revision"
            ],
            "deployment_artifact_id": "cell_a",
            "deployment_revision": "sha256:" + "1" * 64,
            "retained_bundle_id": "sha256:" + "2" * 64,
            "graph_revision": "sha256:" + "3" * 64,
            "program_id": "program_a",
            "program_revision": "sha256:" + "4" * 64,
            "program_binding_id": "sha256:" + "5" * 64,
        }


def test_deployment_candidate_compilation_requires_and_replays_exact_claim() -> None:
    core = FakeCore()
    manager = FakeManager()
    compiler = FakeDeploymentCompiler()
    client = TestClient(
        create_app(service=_service(core, manager, deployment_compiler=compiler))
    )
    body = {
        "scenario_id": "cell_a",
        "expected_scenario_catalog_revision": "sha256:" + "6" * 64,
        "deployment": {"api_version": "manyforge.deployment.v1"},
        "program": {"api_version": "manyforge.program.v2"},
        "authoring_claim": _authoring_claim(),
    }

    response = client.post(
        "/api/manager/deployment-candidates/compile", json=body
    )

    assert response.status_code == 200, response.text
    assert response.json()["compilation"]["scenario_id"] == "cell_a"
    assert compiler.calls == [{
        "scenario_id": "cell_a",
        "expected_scenario_catalog_revision": "sha256:" + "6" * 64,
        "deployment": {"api_version": "manyforge.deployment.v1"},
        "program": {"api_version": "manyforge.program.v2"},
    }]

    body["authoring_claim"] = {"intent": {"claim_kind": "not-authoring"}}
    rejected = client.post(
        "/api/manager/deployment-candidates/compile", json=body
    )
    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == "authoring_not_ready"


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
            "binding_routes": [{
                "control_binding_id": "binding.arm",
                "control_binding_revision": "binding.revision.1",
                "control_route_id": "route.normal",
            }],
        },
        "expected_active_control_configuration": {
            "presence": "absent",
            "active_control_configuration_id": None,
            "active_control_configuration_generation": None,
        },
        "component_expectations": [{
            "controllable_component_id": "component.arm",
            "expected_component_lifecycle_generation": 1,
            "expected_component_lifecycle_state": "prepared",
        }],
        "confirmed": True,
    }


def test_route_selection_requires_current_idle_admission_at_backend_boundary() -> None:
    core = FakeCore()
    manager = FakeManager()
    cclc = FakeCCLC()
    app = create_app(
        service=_service(core, manager, cclc=cclc),
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

    assert stale.status_code == 409
    assert stale.json()["detail"]["code"] == "authoring_not_ready"
    assert accepted.status_code == 200
    assert len(cclc.routes) == 1
    assert "expected_admission_generation" not in cclc.routes[0]


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
        scenarios = client.get("/api/scenarios").json()
        assert scenarios["api_version"] == "manyforge.scenario_catalog.v1"
        assert [row["scenario_id"] for row in scenarios["scenarios"]] == [
            "cell-pick"
        ]

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
        assistant_mode = client.get(
            "/api/assistant/modes/composer-assistant"
        ).json()
        assert assistant_mode["mode"] == "composer-assistant"
        assert assistant_mode["retained_bundle_id"] == "bundle.1"
        assistant_context = client.get(
            "/api/assistant/modes/composer-assistant/context"
        ).json()
        assert assistant_context["schema_version"] == (
            "manyforge.assistant_mode_context.v1"
        )
        assert assistant_context["assistant_mode"]["mode"] == (
            "composer-assistant"
        )
        assert client.get("/api/scene").status_code == 200
        assert client.get("/api/execution/alarms").json()[
            "requested_limit"
        ] == 32
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
        assert overview["schema_version"] == "manyforge.component_overview.v1"
        assert overview["readiness"] == "degraded"
        assert overview["overview_revision"].startswith("sha256:")
        assert next(
            row for row in overview["components"] if row["id"] == "observe_render"
        )["status"] == "healthy"


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
    assert runner.calls == [("node_availability", {
        "applied": applied,
        "selection": core.scenario_program_selection(applied["scenario_id"]),
    })]


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
    runner_projection["source"]["content_digest"] = "sha256:" + hashlib.sha256(
        rfc8785.dumps({
            "availability": runner_projection["availability"],
            **source,
        })
    ).hexdigest()
    service = _service(
        core,
        FakeManager(),
        program_runner=_NodeAvailabilityRunner(runner_projection),
    )

    with pytest.raises(HeadlessApiError) as stale:
        service.node_environment()

    assert stale.value.code == "program_runner_response_invalid"


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


def test_program_apply_skips_unchanged_deployment_and_publishes_exact_program() -> None:
    client, core, manager = _client()
    core.program_target_bundle = "sha256:" + "b" * 64
    core.program_target_graph = "sha256:" + "c" * 64
    core.manager_payload["active"]["retained_bundle_id"] = (
        core.program_target_bundle
    )
    core.manager_payload["active"]["graph_revision"] = core.program_target_graph
    program = copy.deepcopy(_program_document())
    body = {
        "scenario_id": "cell-pick",
        "scenario_catalog_revision": "sha256:" + "6" * 64,
        "program": program,
        "expected_applied_record_digest": None,
        "authoring_claim": _authoring_claim(),
    }
    with client:
        absent = client.get("/api/program/applied")
        response = client.post("/api/program/apply", json=body)
        core.manager_payload["manager_ready"] = False
        replayed = client.post("/api/program/apply", json=body)
        applied = client.get("/api/program/applied")

    assert absent.status_code == 404
    assert response.status_code == 200
    assert replayed.status_code == 200
    assert replayed.json() == response.json()
    assert response.json()["status"] == "applied"
    assert set(response.json()) == {
        "applied",
        "preparation",
        "schema_version",
        "status",
    }
    assert manager.requests == []
    assert core.program_preparations == [program]
    assert len(core.program_commits) == 1
    assert applied.status_code == 200
    assert applied.json() == response.json()["applied"]
    assert applied.json()["program_revision"] == program["program_revision"]


def test_program_apply_rejects_rehashed_malformed_applied_binding_result() -> None:
    core = FakeCore()
    core.program_target_bundle = "sha256:" + "b" * 64
    core.program_target_graph = "sha256:" + "c" * 64
    core.manager_payload["active"]["retained_bundle_id"] = (
        core.program_target_bundle
    )
    core.manager_payload["active"]["graph_revision"] = core.program_target_graph
    original_commit = core.commit_program_apply

    def malformed_commit(
        preparation: dict[str, Any],
        expected_applied_record_digest: str | None,
        request_owner_epoch: str,
        request_id: str,
    ) -> dict[str, Any]:
        result = original_commit(
            preparation,
            expected_applied_record_digest,
            request_owner_epoch,
            request_id,
        )
        binding = result["program_binding"]
        binding["target_role_bindings"][0]["frame_role_bindings"][0][
            "unknown"
        ] = True
        binding["program_binding_id"] = compute_content_identity(
            binding,
            own_field="program_binding_id",
            identity_domain="program-binding-id",
            api_version="manyforge.program_binding.v1",
        )
        result["program_binding_id"] = binding["program_binding_id"]
        return result

    core.commit_program_apply = malformed_commit  # type: ignore[method-assign]
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.post(
        "/api/program/apply",
        json={
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": "sha256:" + "6" * 64,
            "program": copy.deepcopy(_program_document()),
            "expected_applied_record_digest": None,
            "authoring_claim": _authoring_claim(),
        },
    )

    assert response.status_code == 502
    assert response.json()["detail"]["code"] == "program_apply_response_invalid"
    assert "effect_disposition" not in response.json()["detail"]["details"]


def test_program_apply_recovers_exact_commit_before_returning_failure() -> None:
    core = FakeCore()
    core.program_target_bundle = "sha256:" + "b" * 64
    core.program_target_graph = "sha256:" + "c" * 64
    core.manager_payload["active"]["retained_bundle_id"] = (
        core.program_target_bundle
    )
    core.manager_payload["active"]["graph_revision"] = core.program_target_graph
    original_commit = core.commit_program_apply

    def commit_then_lose_result(*args, **kwargs):
        original_commit(*args, **kwargs)
        raise HeadlessApiError(
            "program_apply_commit_failed",
            "fixture lost the direct commit result",
        )

    core.commit_program_apply = commit_then_lose_result  # type: ignore[method-assign]
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.post(
        "/api/program/apply",
        json={
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": "sha256:" + "6" * 64,
            "program": copy.deepcopy(_program_document()),
            "expected_applied_record_digest": None,
            "authoring_claim": _authoring_claim(),
        },
    )

    assert response.status_code == 200
    assert response.json()["status"] == "applied"
    assert len(core.program_commits) == 1


def test_program_apply_commit_rejection_carries_owner_no_effect_proof() -> None:
    core = FakeCore()
    core.program_target_bundle = "sha256:" + "b" * 64
    core.program_target_graph = "sha256:" + "c" * 64
    core.manager_payload["active"]["retained_bundle_id"] = (
        core.program_target_bundle
    )
    core.manager_payload["active"]["graph_revision"] = core.program_target_graph

    def reject_commit(*_args, **_kwargs):
        raise HeadlessApiError(
            "program_apply_selection_stale",
            "fixture rejected the selection before publication",
        )

    core.commit_program_apply = reject_commit  # type: ignore[method-assign]
    client = TestClient(create_app(service=_service(core, FakeManager())))

    response = client.post(
        "/api/program/apply",
        json={
            "scenario_id": "cell-pick",
            "scenario_catalog_revision": "sha256:" + "6" * 64,
            "program": copy.deepcopy(_program_document()),
            "expected_applied_record_digest": None,
            "authoring_claim": _authoring_claim(),
        },
    )

    assert response.status_code == 409
    assert response.json()["detail"]["details"] == {
        "effect_disposition": "proven_no_effect"
    }
    assert core.applied_program is None


def test_program_apply_fences_published_full_stack_restart_before_prepare() -> None:
    core = FakeCore()
    core.manager_payload["apply"] = {
        "mutation_available": False,
        "records": [],
        "unavailable_reason_code": "full_stack_restart_required",
    }
    manager = FakeManager()
    client = TestClient(create_app(service=_service(core, manager)))

    with client:
        response = client.post(
            "/api/program/apply",
            json={
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": "sha256:" + "6" * 64,
                "program": _program_document(),
                "expected_applied_record_digest": None,
                "authoring_claim": _authoring_claim(),
            },
        )

    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "full_stack_restart_required"
    assert core.program_preparations == []
    assert core.program_commits == []
    assert manager.requests == []


def test_program_apply_fences_full_stack_restart_raced_by_preparation() -> None:
    core = FakeCore()
    original_prepare = core.prepare_program_apply

    def prepare_then_publish_restart(
        scenario_id: str,
        scenario_catalog_revision: str,
        program: dict[str, Any],
    ) -> dict[str, Any]:
        preparation = original_prepare(
            scenario_id, scenario_catalog_revision, program
        )
        core.manager_payload["apply"] = {
            "mutation_available": False,
            "records": [],
            "unavailable_reason_code": "full_stack_restart_required",
        }
        return preparation

    core.prepare_program_apply = (  # type: ignore[method-assign]
        prepare_then_publish_restart
    )
    manager = FakeManager()
    client = TestClient(create_app(service=_service(core, manager)))

    with client:
        response = client.post(
            "/api/program/apply",
            json={
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": "sha256:" + "6" * 64,
                "program": _program_document(),
                "expected_applied_record_digest": None,
                "authoring_claim": _authoring_claim(),
            },
        )

    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "full_stack_restart_required"
    assert len(core.program_preparations) == 1
    assert core.program_commits == []
    assert manager.requests == []


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
        "scenario_catalog_revision": "sha256:" + "6" * 64,
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
        selection = client.get(
            "/api/scenarios/cell-pick/program-selection"
        )
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

        assert selection.status_code == 200
        assert selection.json()["scenario_id"] == "cell-pick"
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
        "scenario_catalog_revision": "sha256:" + "6" * 64,
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
        "scenario_catalog_revision": "sha256:" + "6" * 64,
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
    assert stale.json()["detail"]["code"] == (
        "program_run_applied_selection_stale"
    )
    assert divergent_response.status_code == 502
    assert divergent_response.json()["detail"]["code"] == (
        "program_runner_response_invalid"
    )


def test_program_only_apply_does_not_mutate_an_active_captured_run() -> None:
    core = FakeCore()
    core.program_target_bundle = "sha256:" + "b" * 64
    core.program_target_graph = "sha256:" + "c" * 64
    core.manager_payload["active"]["retained_bundle_id"] = (
        core.program_target_bundle
    )
    core.manager_payload["active"]["graph_revision"] = (
        core.program_target_graph
    )
    runner = FakeProgramRunner()
    app = create_app(
        service=_service(
            core,
            FakeManager(),
            program_runner=runner,
        )
    )
    client = TestClient(app)

    with client:
        response = client.post(
            "/api/program/apply",
            json={
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": "sha256:" + "6" * 64,
                "program": _program_document(),
                "expected_applied_record_digest": None,
                "authoring_claim": _authoring_claim(),
            },
        )

    assert response.status_code == 200, response.text
    assert response.json()["status"] == "applied"
    assert len(core.program_preparations) == 1
    assert [call[0] for call in runner.calls] == []


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


def test_program_apply_rejects_inactive_target_without_artifacts_or_manager_apply(
) -> None:
    core = FakeCore()
    core.program_target_bundle = "sha256:" + "b" * 64
    core.program_target_graph = "sha256:" + "c" * 64
    manager = FakeManager()
    client = TestClient(create_app(service=_service(core, manager)))
    body = {
        "scenario_id": "cell-pick",
        "scenario_catalog_revision": "sha256:" + "6" * 64,
        "program": copy.deepcopy(_program_document()),
        "expected_applied_record_digest": None,
        "authoring_claim": _authoring_claim(),
    }

    with client:
        rejected = client.post("/api/program/apply", json=body)
        assert client.get("/api/program/applied").status_code == 404

    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == (
        "program_apply_deployment_not_active"
    )
    assert rejected.json()["detail"]["details"] == {
        "effect_disposition": "proven_no_effect"
    }
    assert core.program_preparations == []
    assert core.program_commits == []
    assert manager.requests == []


def test_program_apply_inactive_target_preserves_prior_applied_selection() -> None:
    core = FakeCore()
    prior = _applied_projection(
        _program_document(),
        scenario_id="prior-scenario",
        record_digest="sha256:" + "9" * 64,
    )
    core.applied_program = copy.deepcopy(prior)
    core.program_target_bundle = "sha256:" + "b" * 64
    core.program_target_graph = "sha256:" + "c" * 64
    manager = FakeManager()
    client = TestClient(create_app(service=_service(core, manager)))

    with client:
        rejected = client.post(
            "/api/program/apply",
            json={
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": "sha256:" + "6" * 64,
                "program": copy.deepcopy(_program_document()),
                "expected_applied_record_digest": prior["source"][
                    "record_digest"
                ],
                "authoring_claim": _authoring_claim(),
            },
        )
        still_applied = client.get("/api/program/applied")

    assert rejected.status_code == 409
    assert rejected.json()["detail"]["code"] == (
        "program_apply_deployment_not_active"
    )
    assert manager.requests == []
    assert core.program_preparations == []
    assert core.program_commits == []
    assert still_applied.status_code == 200
    assert still_applied.json() == prior


def test_program_apply_rejects_invalid_program_before_native_preparation() -> None:
    client, core, _manager = _client()
    with client:
        response = client.post(
            "/api/program/apply",
            json={
                "scenario_id": "cell-pick",
                "scenario_catalog_revision": "sha256:" + "6" * 64,
                "program": {"api_version": "manyforge.program.v1"},
                "expected_applied_record_digest": None,
                "authoring_claim": _authoring_claim(),
            },
        )
    assert response.status_code == 400
    assert response.json()["detail"]["code"] == "program_apply_program_invalid"
    assert response.json()["detail"]["details"] == {
        "effect_disposition": "proven_no_effect"
    }
    assert core.program_preparations == []


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
                "expected_admission_generation": 7,
                "candidate_receipt": {
                    "schema_version": "manyforge.manager_deployment_candidate_receipt.v1",
                    "deployment_artifact_id": "deployment.1",
                    "deployment_revision": "sha256:" + "b" * 64,
                    "scenario_catalog_revision": "sha256:" + "c" * 64,
                    "candidate_retained_bundle_id": "bundle.2",
                    "candidate_graph_revision": "graph.2",
                    "expected_current": {
                        "graph_revision": "graph.1",
                        "retained_bundle_id": "bundle.1",
                    },
                    "expected_admission_generation": 7,
                    "manager_owner_epoch": "client.epoch.1",
                    "receipt_digest": "sha256:" + "d" * 64,
                },
            },
        )
    assert response.status_code == 503
    assert response.json()["error"]["code"] == "kernel_owner_unavailable"
    assert manager.requests[0].source_label == "operator_ui"


def test_apply_status_is_a_read_only_durable_identity_observation() -> None:
    client, _core, manager = _client()
    with client:
        response = client.get(
            "/api/manager/apply/status",
            params={"apply_id": "apply/7", "apply_generation": 7},
        )

    assert response.status_code == 200
    assert response.json()["state"] == "pending"
    assert manager.apply_observations == [("apply/7", 7)]


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
    assert "support_contacts" in openapi["components"]["schemas"][
        "AddSceneObjectRequest"
    ]["properties"]
    assert "body_a" not in openapi["components"]["schemas"][
        "SceneSupportContactRequest"
    ]["properties"]


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


def test_program_apply_large_artifact_envelope_does_not_widen_other_routes() -> None:
    client, _core, _manager = _client()
    body = b" " * (1024 * 1024) + b"{}"
    with client:
        program_response = client.post(
            "/api/program/apply",
            content=body,
            headers={"content-type": "application/json"},
        )
        manager_response = client.post(
            "/api/manager/apply",
            content=body,
            headers={"content-type": "application/json"},
        )

    assert program_response.status_code == 422, program_response.text
    assert manager_response.status_code == 413, manager_response.text


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


def test_recognized_malformed_stop_promotes_instead_of_returning_validation_error() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(FakeCore(), FakeManager(), execution)
    )
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
    assert "/api/scenarios" in paths
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
        "/api/execution/pick-place/sequences/pick",
        "/api/execution/pick-place/sequences/place",
        "/api/execution/pick-place/sequences/{sequence_id}",
    }.issubset(paths)
    assert "/api/kernel/stop" not in paths
    assert "/api/cycle/stop" not in paths
    assert all("condition" not in path or "acknowledge" in path for path in paths)


def test_execution_diagnostics_is_explicit_and_status_is_only_compatibility() -> None:
    execution = FakeExecution()
    app = create_app(service=_service(FakeCore(), FakeManager(), execution))
    with TestClient(app) as client:
        diagnostics = client.get("/api/execution/diagnostics")
        compatibility = client.get("/api/execution/status")

    assert diagnostics.status_code == 200
    assert diagnostics.json() == compatibility.json() == {
        "ok": True,
        "command": "status",
        "status": {},
    }


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


def test_execution_observation_websocket_proxies_one_persistent_credited_session() -> None:
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
                    "schema_version": "manyforge.execution_observation.v2",
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
                    "schema_version": (
                        "manyforge.execution_observation_credit.v1"
                    ),
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


def test_motion_preview_is_a_thin_non_effectful_execution_route() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(FakeCore(), FakeManager(), execution)
    )
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
        response = client.post("/api/execution/motion-previews", json=body)

    assert response.status_code == 200, response.text
    assert response.json()["command"] == "preview_motion"
    assert [kind for kind, _request in execution.requests] == [
        "preview_motion"
    ]
    assert execution.principals == [("preview_motion", None)]


def test_capability_command_preview_is_a_thin_non_effectful_execution_route() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(FakeCore(), FakeManager(), execution)
    )
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
        response = client.post(
            "/api/execution/capability-command-previews", json=body
        )
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

    app = create_app(
        service=_service(FakeCore(), FakeManager(), MissingExecution())
    )
    with TestClient(app) as client:
        response = client.get("/api/execution/operations/operation-retired")

    assert response.status_code == 404
    assert response.json()["error"]["code"] == "missing_reference"


def test_operation_controls_and_scale_control_are_thin_fenced_routes() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(
            FakeCore(), FakeManager(), execution,
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
        assert client.post(
            "/api/execution/runtime-scale-control", json=scale_control
        ).status_code == 200
        assert client.get(
            "/api/execution/runtime-scale-control",
            params=scale_control["selector"],
        ).status_code == 200

    assert [kind for kind, _request in execution.requests] == [
        "pause",
        "resume",
        "reset",
        "acknowledge_completed",
        "stop",
        "set_runtime_scale_control",
        "get_runtime_scale_control",
    ]
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
            FakeCore(), FakeManager(), execution,
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
        recovered = client.post(
            "/api/execution/stop-fault/recover", json=recovery
        )

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
            FakeCore(), FakeManager(), execution,
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
        accepted = client.post(
            "/api/execution/operations/operation-1/pause", json=body
        )

    assert forged.status_code == 422
    assert forged_context.status_code == 422
    assert accepted.status_code == 200
    assert execution.principals == [("pause", "operator.7")]


def test_operation_control_paths_accept_bounded_stop_and_promote_mixed_scope() -> None:
    execution = FakeExecution()
    app = create_app(
        service=_service(
            FakeCore(), FakeManager(), execution,
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
    assert mismatch.json()["detail"]["code"] == (
        "execution_operation_path_invalid"
    )
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
