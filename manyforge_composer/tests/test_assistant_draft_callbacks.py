"""End-to-end Draft callback semantics over verified owner projections."""

from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path

from fastapi.testclient import TestClient

from manyforge_behavior.canonical_artifact import (
    CONTENT_IDENTITY_PREFIX,
    canonical_json_bytes,
    raw_sha256_digest,
)
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.program import Program
from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.assistant_catalog_narrowing import (
    resolve_assistant_catalog,
)
from manyforge_composer.backend.assistant_mode_client import (
    VerifiedActiveAssistantContext,
    VerifiedActiveAssistantMode,
    verify_active_assistant_mode,
)
from manyforge_composer.backend.node_environment_client import (
    VerifiedActiveNodeEnvironment,
)
from manyforge_composer.backend.source_model_client import verify_active_source_model
from manyforge_composer.backend.state import ComposerState


_REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
_PROGRAM_PATH = (
    _REPOSITORY_ROOT / "examples" / "pick_and_place_ur10e_robotiq.program.yaml"
)
_ASSISTANT_FORBIDDEN_PROGRAM_PATH = (
    _REPOSITORY_ROOT / "examples" / "pick_and_place_minimal.program.yaml"
)
_REGISTRY_PATH = (
    _REPOSITORY_ROOT
    / "manyforge_deployment"
    / "resources"
    / "assistant"
    / "assistant_instrument_registry_source.v1.json"
)
_MANAGER_DIGEST = "sha256:" + "1" * 64
_NODE_PROJECTION_DIGEST = "sha256:" + "2" * 64
_ASSISTANT_PROJECTION_DIGEST = "sha256:" + "4" * 64
_REGISTRY_DIGEST = "sha256:" + "5" * 64


def _framed(domain: str, version: str, value: dict) -> str:
    return raw_sha256_digest(
        b"\0".join(
            (
                CONTENT_IDENTITY_PREFIX,
                domain.encode("ascii"),
                version.encode("ascii"),
                canonical_json_bytes(value),
            )
        )
    )


def _selection_source(*, content_digest: str, owner_kind: str) -> dict:
    return {
        "available": True,
        "freshness": "fresh",
        "known_gaps": [],
        "projection_generation": 7,
        "projection_owner_epoch": "headless.epoch.1",
        "receiver_age_ns": 10,
        "selection_projection_generation": 5,
        "selection_source_content_digest": _MANAGER_DIGEST,
        "selection_source_generation": 3,
        "selection_source_owner_epoch": "manager.epoch.1",
        "selection_source_owner_id": "component-manager.cell-a",
        "source_content_digest": content_digest,
        "source_generation": 7,
        "source_owner_epoch": "headless.epoch.1",
        "source_owner_id": f"headless-api.{owner_kind}",
        "source_owner_kind": owner_kind,
    }


def _node_environment() -> VerifiedActiveNodeEnvironment:
    environment = build_node_environment()
    manifest = environment.manifest
    return VerifiedActiveNodeEnvironment(
        payload={
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
            "source": _selection_source(
                content_digest=_NODE_PROJECTION_DIGEST,
                owner_kind="node_environment_projection",
            ),
        },
        catalog=environment.catalog,
        manifest=manifest,
    )


def _source_model(node_environment: VerifiedActiveNodeEnvironment):
    projection = {
        "schema_version": "manyforge.source_model_projection.v2",
        "cell_id": "cell-a",
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "model_map_contract": None,
        "model_map_product_id": None,
        "occurrences": [],
        "targets": [],
        "source": _selection_source(
            content_digest="sha256:" + "0" * 64,
            owner_kind="source_model_projection",
        ),
    }
    projection["source"]["source_content_digest"] = _framed(
        "manyforge-source-model-projection-semantic",
        "manyforge.source_model_projection.v2",
        {key: value for key, value in projection.items() if key != "source"},
    )
    return verify_active_source_model(
        projection,
        node_environment=node_environment,
    )


def _assistant_context(
    node_environment: VerifiedActiveNodeEnvironment,
) -> VerifiedActiveAssistantContext:
    return VerifiedActiveAssistantContext(
        node_environment=node_environment,
        source_model=_source_model(node_environment),
        assistant_mode=_assistant_mode(node_environment),
    )


def _assistant_mode(
    node_environment: VerifiedActiveNodeEnvironment,
) -> VerifiedActiveAssistantMode:
    registry = json.loads(_REGISTRY_PATH.read_text(encoding="utf-8"))
    source_tool = next(
        row for row in registry["tools"] if row["tool_id"] == "scene_draft_add_object"
    )
    schema = source_tool["input_schema"]
    tool = {
        "description": source_tool["description"],
        "effect": source_tool["effect"],
        "input_schema": schema,
        "input_schema_hash": raw_sha256_digest(canonical_json_bytes(schema)),
        "tool_id": source_tool["tool_id"],
    }
    policy = {
        "approval_mode": None,
        "review_policy": "required",
        "session_kind": None,
    }
    catalog = {
        "grant_policy": policy,
        "mode": "composer-assistant",
        "nodes": [],
        "skills": [],
        "tools": [tool],
    }
    environment = node_environment.payload
    projection = {
        "schema_version": "manyforge.assistant_mode_projection.v1",
        "assistant_projection_digest": _ASSISTANT_PROJECTION_DIGEST,
        "catalog_hash": _framed(
            "assistant-mode-catalog",
            "manyforge.assistant_mode_catalog.v1",
            catalog,
        ),
        "description": "Draft callback test mode.",
        "grant_policy": policy,
        "graph_revision": "graph.1",
        "instrument_registry_digest": _REGISTRY_DIGEST,
        "mode": "composer-assistant",
        "model_endpoint": {
            "ai_model_component_id": "assistant_model",
            "api_key_env": None,
            "base_url": "http://127.0.0.1:8000/v1",
            "model_endpoint_alias": "assistant_model",
            "model_id": "test-model",
            "request_contract": {
                "default_timeout_s": 30,
                "max_output_tokens": 512,
                "supported_image_transports": [],
                "supports_image_input": False,
                "supports_json_object": True,
                "supports_response_tools": True,
                "tool_call_family": "openai_chat_tools",
            },
        },
        "node_environment": dict(environment["package_environment"]),
        "nodes": [],
        "provider": {
            "provider_alias": "assistant_provider",
            "provider_kind": "direct",
        },
        "retained_bundle_id": "bundle.1",
        "skills": [],
        "tools": [tool],
    }
    projection_digest = _framed(
        "manyforge-assistant-mode-projection-semantic",
        "manyforge.assistant_mode_projection.v1",
        projection,
    )
    projection["source"] = _selection_source(
        content_digest=projection_digest,
        owner_kind="assistant_mode_projection",
    )
    return verify_active_assistant_mode(
        projection,
        expected_mode="composer-assistant",
        node_environment=node_environment,
    )


def _resource(identity: str = "workpiece") -> dict:
    return {
        "scene_resource_id": identity,
        "shape": {"type": "box", "box_dimensions_m": [0.1, 0.2, 0.3]},
        "pose": {
            "frame_ref": {"kind": "cell_frame"},
            "position_m": [0.0, 0.0, 0.0],
            "orientation": {"xyzw": [0.0, 0.0, 0.0, 1.0]},
        },
    }


class _AdmissionHeadless:
    def __init__(self) -> None:
        self.claims: list[dict] = []
        self.settlements: list[dict] = []
        self.generation = 7

    def authoring_admission(self) -> dict:
        return {
            "authoring_allowed": True,
            "admission_generation": self.generation,
        }

    def acquire_authoring_transaction(self, body: dict) -> dict:
        self.claims.append(deepcopy(body))
        acquired_generation = self.generation
        self.generation += 1
        return {
            "claim": {
                "intent": {
                    "claim_kind": "authoring_mutation",
                    "owner_role": "authoring_owner",
                    **body,
                },
                "claim_generation": 1,
                "acquired_admission_generation": acquired_generation,
            }
        }

    def settle_authoring_transaction(self, _body: dict) -> dict:
        self.settlements.append(deepcopy(_body))
        self.generation += 1
        return {
            "admission": {
                "authoring_allowed": True,
                "admission_generation": self.generation,
            }
        }

    def recover_authoring_transaction(self, _body: dict) -> dict:
        return {
            "recovery_receipt": {
                "status": "unknown",
                "kernel_owner_epoch": "kernel.1",
                "held_claims": [],
                "current_owner_epochs": [],
            }
        }

    def reconcile_authoring_owner(self, _body: dict) -> dict:
        return {"admission": {"admission_generation": 7}}


def _body(
    state: ComposerState,
    node_environment: VerifiedActiveNodeEnvironment,
    source_model,
    assistant_mode: VerifiedActiveAssistantMode,
) -> dict:
    snapshot = state.draft_session.snapshot()
    workspace = state.authoring_workspace.identity()
    mode = assistant_mode.payload
    return {
        "assistantMode": "composer-assistant",
        "retainedBundleId": "bundle.1",
        "graphRevision": "graph.1",
        "nodeEnvironmentDigest": node_environment.payload["environment_digest"],
        "nodeEnvironmentProjectionDigest": _NODE_PROJECTION_DIGEST,
        "sourceModelProjectionDigest": source_model.payload["source"][
            "source_content_digest"
        ],
        "assistantProjectionDigest": _ASSISTANT_PROJECTION_DIGEST,
        "assistantModeProjectionDigest": mode["source"]["source_content_digest"],
        "catalogHash": mode["catalog_hash"],
        "requestedTools": [],
        "fieldPolicy": {},
        "inputSchemaHash": mode["tools"][0]["input_schema_hash"],
        "draftId": snapshot["draft_id"],
        "expectedDraftRevision": snapshot["draft_revision"],
        "expectedWorkspaceEpoch": workspace.workspace_epoch,
        "expectedWorkspaceRevision": workspace.workspace_revision,
        "requestId": "assistant.request.1",
        "requestOwnerEpoch": "assistant.epoch.1",
        "expectedAdmissionGeneration": 7,
        "sourceLabel": "assistant_bridge",
        "correlationId": None,
        "arguments": {"scene_resource": _resource()},
    }


def test_callback_uses_exact_active_context_and_mutates_only_the_draft(
    monkeypatch, tmp_path
) -> None:
    node_environment = _node_environment()
    context = _assistant_context(node_environment)
    source_model = context.source_model
    assistant_mode = context.assistant_mode
    environment = build_node_environment()
    program = Program(_PROGRAM_PATH, environment.catalog)
    state = ComposerState(
        node_environment=environment,
        program=program,
        program_source_path=str(_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    from manyforge_composer.backend import routes_assistant_draft as route

    projection_reads = 0

    def read_context(_state, *, mode):
        nonlocal projection_reads
        projection_reads += 1
        return context

    monkeypatch.setattr(
        route,
        "read_active_assistant_context",
        read_context,
    )
    client = TestClient(create_app(state))
    body = _body(state, node_environment, source_model, assistant_mode)

    response = client.post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=body,
    )

    assert response.status_code == 200
    result = response.json()
    assert result["success"] is True
    assert result["mutated"] is True
    assert result["draftMutated"] is True
    assert result["nextAdmissionGeneration"] == 9
    assert result["result"]["draftRevision"] == 2
    assert projection_reads == 2
    assert state.draft_session.snapshot()["applied"] is False
    assert state.draft_session.snapshot()["saved"] is False
    assert state.authoring_admission is not None
    assert not hasattr(state.authoring_admission, "_snapshots")
    assert not hasattr(state.authoring_admission, "_journal")

    second_body = deepcopy(body)
    second_body["expectedDraftRevision"] = 2
    second_body["expectedAdmissionGeneration"] = result[
        "nextAdmissionGeneration"
    ]
    second_body["expectedWorkspaceRevision"] = result[
        "nextWorkspaceRevision"
    ]
    second_body["requestId"] = "assistant.request.2"
    second_body["arguments"]["scene_resource"][
        "scene_resource_id"
    ] = "second-workpiece"
    second = client.post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=second_body,
    )
    assert second.status_code == 200
    assert second.json()["result"]["draftRevision"] == 3
    assert second.json()["nextAdmissionGeneration"] == 11

    # Ordinary in-memory edits have no durable replay ledger. A lost-response
    # retry carries the old workspace fence and is rejected as stale instead
    # of repeating the edit.
    replay = client.post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=body,
    )
    assert replay.status_code == 409
    assert replay.json()["detail"]["code"] == "composer_workspace_stale"
    assert len(state.headless_client.claims) == 2
    assert len(state.headless_client.settlements) == 2

    conflicting = deepcopy(body)
    conflicting["arguments"]["scene_resource"]["scene_resource_id"] = "other"
    conflict = client.post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=conflicting,
    )
    assert conflict.status_code == 409
    assert conflict.json()["detail"]["code"] == "composer_workspace_stale"


def test_allowed_tool_schema_error_is_a_non_mutating_http_200_result(
    monkeypatch, tmp_path
) -> None:
    node_environment = _node_environment()
    context = _assistant_context(node_environment)
    source_model = context.source_model
    assistant_mode = context.assistant_mode
    environment = build_node_environment()
    state = ComposerState(
        node_environment=environment,
        program=Program(_PROGRAM_PATH, environment.catalog),
        program_source_path=str(_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    from manyforge_composer.backend import routes_assistant_draft as route

    monkeypatch.setattr(
        route,
        "read_active_assistant_context",
        lambda _state, *, mode: context,
    )
    body = _body(state, node_environment, source_model, assistant_mode)
    body["arguments"]["scene_resource"]["objectId"] = body["arguments"][
        "scene_resource"
    ].pop("scene_resource_id")
    before = deepcopy(state.draft_session.snapshot())

    response = TestClient(create_app(state)).post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=body,
    )

    assert response.status_code == 200
    result = response.json()
    assert result["success"] is False
    assert result["mutated"] is False
    assert result["draftMutated"] is False
    assert result["nextAdmissionGeneration"] == 7
    assert result["result"]["kind"] == "validation_error"
    assert result["result"]["httpStatus"] == 400
    assert state.draft_session.snapshot() == before
    assert state.headless_client.claims == []


def test_callback_reapplies_field_narrowing_before_draft_mutation(
    monkeypatch, tmp_path
) -> None:
    node_environment = _node_environment()
    context = _assistant_context(node_environment)
    source_model = context.source_model
    assistant_mode = context.assistant_mode
    environment = build_node_environment()
    state = ComposerState(
        node_environment=environment,
        program=Program(_PROGRAM_PATH, environment.catalog),
        program_source_path=str(_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    from manyforge_composer.backend import routes_assistant_draft as route

    monkeypatch.setattr(
        route,
        "read_active_assistant_context",
        lambda _state, *, mode: context,
    )
    body = _body(state, node_environment, source_model, assistant_mode)
    body["fieldPolicy"] = {"allowedOrientationFields": ["rpy_deg"]}
    narrowed = resolve_assistant_catalog(
        assistant_mode.payload,
        requested_tools=[],
        field_policy=body["fieldPolicy"],
    )
    body["catalogHash"] = narrowed["catalogHash"]
    body["inputSchemaHash"] = narrowed["tools"][0]["input_schema_hash"]
    before = deepcopy(state.draft_session.snapshot())

    response = TestClient(create_app(state)).post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=body,
    )

    assert response.status_code == 200
    assert response.json()["success"] is False
    assert response.json()["result"]["message"].startswith(
        "$.scene_resource.pose.orientation: value must match exactly one schema branch"
    )
    assert state.draft_session.snapshot() == before


def test_malformed_assistant_mode_is_rejected_before_projection_lookup(
    monkeypatch, tmp_path
) -> None:
    node_environment = _node_environment()
    source_model = _source_model(node_environment)
    assistant_mode = _assistant_mode(node_environment)
    environment = build_node_environment()
    state = ComposerState(
        node_environment=environment,
        program=Program(_PROGRAM_PATH, environment.catalog),
        program_source_path=str(_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    from manyforge_composer.backend import routes_assistant_draft as route

    lookup_attempted = False

    def unexpected_lookup(*_args, **_kwargs):
        nonlocal lookup_attempted
        lookup_attempted = True
        raise AssertionError("projection lookup must not run")

    monkeypatch.setattr(route, "read_active_assistant_context", unexpected_lookup)
    body = _body(state, node_environment, source_model, assistant_mode)
    body["assistantMode"] = "../runtime"
    before = deepcopy(state.draft_session.snapshot())

    response = TestClient(create_app(state)).post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=body,
    )

    assert response.status_code == 422
    assert lookup_attempted is False
    assert state.draft_session.snapshot() == before


def test_callback_cannot_mutate_a_program_that_forbids_assistant_authoring(
    monkeypatch, tmp_path
) -> None:
    node_environment = _node_environment()
    context = _assistant_context(node_environment)
    source_model = context.source_model
    assistant_mode = context.assistant_mode
    environment = build_node_environment()
    state = ComposerState(
        node_environment=environment,
        program=Program(_ASSISTANT_FORBIDDEN_PROGRAM_PATH, environment.catalog),
        program_source_path=str(_ASSISTANT_FORBIDDEN_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    from manyforge_composer.backend import routes_assistant_draft as route

    monkeypatch.setattr(
        route,
        "read_active_assistant_context",
        lambda _state, *, mode: context,
    )
    body = _body(state, node_environment, source_model, assistant_mode)
    before = deepcopy(state.draft_session.snapshot())

    response = TestClient(create_app(state)).post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=body,
    )

    assert response.status_code == 409
    assert response.json()["detail"]["code"] == (
        "assistant_callback_program_incompatible"
    )
    assert state.draft_session.snapshot() == before


def test_assistant_projection_verifier_rejects_schema_or_catalog_drift() -> None:
    node_environment = _node_environment()
    valid = _assistant_mode(node_environment).payload

    stale_schema = deepcopy(valid)
    stale_schema["tools"][0]["input_schema_hash"] = "sha256:" + "0" * 64
    try:
        verify_active_assistant_mode(
            stale_schema,
            expected_mode="composer-assistant",
            node_environment=node_environment,
        )
        raise AssertionError("stale schema hash was accepted")
    except RuntimeError as error:
        assert "input_schema_hash" in str(error)

    stale_catalog = deepcopy(valid)
    stale_catalog["catalog_hash"] = "sha256:" + "0" * 64
    try:
        verify_active_assistant_mode(
            stale_catalog,
            expected_mode="composer-assistant",
            node_environment=node_environment,
        )
        raise AssertionError("stale catalog hash was accepted")
    except RuntimeError as error:
        assert "catalog_hash" in str(error)
