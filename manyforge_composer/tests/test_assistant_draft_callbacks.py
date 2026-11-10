"""End-to-end Draft callback semantics over verified owner projections."""

from __future__ import annotations

from copy import deepcopy
import json
from pathlib import Path

from fastapi.testclient import TestClient
import pytest

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
    ActiveAssistantModeProjectionError,
    VerifiedActiveAssistantContext,
    VerifiedActiveAssistantMode,
    _model_request_contract,
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
        "schema_version": "manyforge.source_model_projection.v3",
        "cell_id": "cell-a",
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "model_map_contract": None,
        "model_map_product_id": None,
        "components": [],
        "occurrences": [],
        "targets": [],
        "source": _selection_source(
            content_digest="sha256:" + "0" * 64,
            owner_kind="source_model_projection",
        ),
    }
    projection["source"]["source_content_digest"] = _framed(
        "manyforge-source-model-projection-semantic",
        "manyforge.source_model_projection.v3",
        {key: value for key, value in projection.items() if key != "source"},
    )
    return verify_active_source_model(
        projection,
        node_environment=node_environment,
    )


def _assistant_context(
    node_environment: VerifiedActiveNodeEnvironment,
    tool_id: str = "scene_draft_add_object",
    node_ids: list[str] | None = None,
) -> VerifiedActiveAssistantContext:
    return VerifiedActiveAssistantContext(
        node_environment=node_environment,
        source_model=_source_model(node_environment),
        assistant_mode=_assistant_mode(node_environment, tool_id, node_ids or []),
    )


def _assistant_mode(
    node_environment: VerifiedActiveNodeEnvironment,
    tool_id: str = "scene_draft_add_object",
    node_ids: list[str] | None = None,
) -> VerifiedActiveAssistantMode:
    from manyforge_headless_api.service import _assistant_instruments

    # The tool descriptor as Headless builds it from the loaded catalog.
    effect = ("read_only" if tool_id.endswith(("_read", "_validate", "_inspect"))
              else "composer_draft_mutating")
    tool = _assistant_instruments()._tool_row(tool_id, effect, node_ids or [])
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
        "schema_version": "manyforge.assistant_mode_projection.v2",
        "assistant_component_id": "assistant",
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
        "retained_bundle_id": "bundle.1",
        "runs_in": ["no_program_active"],
        "skills": [],
        "compact_history": False,
        "constrained_regeneration": False,
        "stuck_loop_recovery": True,
        "tools": [tool],
        "types": _assistant_instruments()._all_types(node_ids or []),
    }
    projection_digest = _framed(
        "manyforge-assistant-mode-projection-semantic",
        "manyforge.assistant_mode_projection.v2",
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

    def program_cycle_status(self) -> dict:
        return {"state": "idle_after_reset"}

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


def test_validate_callback_returns_reference_diagnostics_for_exact_captured_draft(monkeypatch) -> None:
    node_environment = _node_environment()
    context = _assistant_context(node_environment, "program_validate")
    environment = build_node_environment()
    state = ComposerState(
        node_environment=environment,
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    draft = state.draft_session.snapshot()
    draft["program_loaded"] = True
    draft["authoring"] = {"schema_version": "manyforge.composer_authoring_forest.v1", "forest": []}
    monkeypatch.setattr(state.draft_session, "snapshot", lambda: deepcopy(draft))
    monkeypatch.setattr(state.draft_session, "validate", lambda _catalog: deepcopy(draft))
    from manyforge_composer.backend import routes_assistant_draft as route
    monkeypatch.setattr(route, "read_active_assistant_context", lambda _state, *, mode: context)
    captured = []
    reference = {"diagnostics": [{"code": "reference_missing", "path": "/tree/target"}],
                 "total_count": 1, "truncated": False}

    def validate(actual_state, authoring, *, expected_identity):
        captured.append((actual_state, deepcopy(authoring), expected_identity))
        return {"valid": False, "reference_validation": reference}

    monkeypatch.setattr(route, "validate_captured_program_source", validate)
    body = _body(state, node_environment, context.source_model, context.assistant_mode)
    body["arguments"] = {}
    response = TestClient(create_app(state)).post("/api/assistant/bridge/tools/program_validate", json=body)
    assert response.status_code == 200
    result = response.json()
    assert result["mutated"] is False
    assert result["result"]["details"] == {"valid": False, "reference_validation": reference}
    assert captured == [(state, state.draft_session.snapshot()["authoring"], state.authoring_workspace.identity())]
    stale = deepcopy(body)
    stale["expectedWorkspaceRevision"] += 1
    rejected = TestClient(create_app(state)).post("/api/assistant/bridge/tools/program_validate", json=stale)
    assert rejected.status_code == 409
    assert len(captured) == 1


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
    # A Draft edit takes no Kernel claim, so the generation is unchanged.
    assert result["nextAdmissionGeneration"] == 7
    assert state.headless_client.claims == []
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
    assert second.json()["nextAdmissionGeneration"] == 7

    # Ordinary in-memory edits have no durable replay ledger. A lost-response
    # retry carries the old workspace fence and is rejected as stale instead
    # of repeating the edit.
    replay = client.post(
        "/api/assistant/bridge/tools/scene_draft_add_object",
        json=body,
    )
    assert replay.status_code == 409
    assert replay.json()["detail"]["code"] == "composer_workspace_stale"
    assert state.headless_client.claims == []
    assert state.headless_client.settlements == []

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
    assert result["result"]["detail"]["code"] == "assistant_callback_arguments_invalid"
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


def test_real_bridge_reports_claimless_draft_edits_and_refuses_a_stale_fence(
    monkeypatch, tmp_path
) -> None:
    """The real bridge loop drives the real Composer callback route."""

    import asyncio
    import time

    import httpx
    from manyforge_assistant_bridge.bridge import create_app as create_bridge_app
    from manyforge_assistant_bridge.catalog import raw_digest
    from manyforge_assistant_bridge.tests import test_bridge as bridge_fixture

    node_environment = _node_environment()
    context = _assistant_context(node_environment)
    environment = build_node_environment()
    state = ComposerState(
        node_environment=environment,
        program=Program(_PROGRAM_PATH, environment.catalog),
        program_source_path=str(_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    from manyforge_composer.backend import routes_assistant_draft as route

    monkeypatch.setattr(
        route, "read_active_assistant_context", lambda _state, *, mode: context
    )
    composer = TestClient(create_app(state))
    template = _body(
        state, node_environment, context.source_model, context.assistant_mode
    )
    callback_bodies: list[dict] = []
    responses: list[tuple[int, dict]] = []

    async def composer_handler(request: httpx.Request) -> httpx.Response:
        sent = json.loads(request.content)
        callback_bodies.append(sent)
        # The bridge's own fences and identities; the active-context
        # identities and arguments come from Composer's fixture.
        body = deepcopy(template)
        for field in (
            "draftId", "expectedDraftRevision", "expectedWorkspaceEpoch",
            "expectedWorkspaceRevision", "expectedAdmissionGeneration",
            "requestId", "requestOwnerEpoch", "sourceLabel", "correlationId",
        ):
            body[field] = sent[field]
        body["arguments"] = {
            "scene_resource": _resource(f"bridge-part-{len(callback_bodies)}")
        }
        reply = await asyncio.to_thread(
            composer.post,
            "/api/assistant/bridge/tools/scene_draft_add_object",
            json=body,
        )
        responses.append((reply.status_code, reply.json()))
        return httpx.Response(reply.status_code, json=reply.json(), request=request)

    projection = bridge_fixture._projection()
    schema = {"additionalProperties": False, "properties": {}, "type": "object"}
    bridge_fixture._append_projection_tool(
        projection,
        {
            "description": "Add one authored scene object.",
            "effect": "composer_draft_mutating",
            "input_schema": schema,
            "input_schema_hash": raw_digest(schema),
            "tool_id": "scene_draft_add_object",
        },
    )
    turns = 0

    async def upstream(request: httpx.Request) -> httpx.Response:
        nonlocal turns
        turns += 1
        message = {"content": "Done."} if turns % 2 == 0 else {
            "content": "",
            "tool_calls": [
                {
                    "id": f"call.{turns}.{index}",
                    "type": "function",
                    "function": {"name": "scene_draft_add_object", "arguments": "{}"},
                }
                for index in range(2)
            ],
        }
        return httpx.Response(
            200, json={"choices": [{"message": message}]}, request=request
        )

    def provider_request(request_id: str, draft: dict, workspace) -> dict:
        body = bridge_fixture._request(projection)
        body["requestId"] = request_id
        body["composerDraft"].update(
            draftId=draft["draft_id"],
            expectedDraftRevision=draft["draft_revision"],
            expectedWorkspaceEpoch=workspace.workspace_epoch,
            expectedWorkspaceRevision=workspace.workspace_revision,
            expectedAdmissionGeneration=7,
        )
        return body

    def terminal(client: TestClient, request_id: str) -> dict:
        for _ in range(500):
            status = client.get(f"/v1/manyforge/assistant/{request_id}").json()
            if status["state"] != "running":
                return status
            time.sleep(0.01)
        raise AssertionError("assistant request did not finish")

    before_draft = state.draft_session.snapshot()
    before_workspace = state.authoring_workspace.identity()
    service = bridge_fixture._service(
        tmp_path, projection, upstream_handler=upstream,
        composer_handler=composer_handler,
    )
    with TestClient(create_bridge_app(service.config, service=service)) as client:
        assert client.post(
            "/v1/manyforge/assistant",
            json=provider_request("request.1", before_draft, before_workspace),
        ).status_code == 202
        first = terminal(client, "request.1")

        # Two consecutive claimless edits, both committed and reported.
        assert first["state"] == "completed", first
        assert first["response"]["draftMutated"] is True
        assert [code for code, _ in responses] == [200, 200]
        assert all(reply["mutated"] is True for _, reply in responses)
        assert [body["expectedAdmissionGeneration"] for body in callback_bodies] == [7, 7]
        assert [body["expectedWorkspaceRevision"] for body in callback_bodies] == [
            before_workspace.workspace_revision,
            before_workspace.workspace_revision + 1,
        ]
        assert state.draft_session.snapshot()["draft_revision"] == (
            before_draft["draft_revision"] + 2
        )
        assert state.authoring_workspace.identity().workspace_revision == (
            before_workspace.workspace_revision + 2
        )
        assert state.headless_client.claims == []

        # A request built on the old workspace is refused by the fence and
        # changes nothing; the bridge never reports it as committed.
        after = state.draft_session.snapshot()
        assert client.post(
            "/v1/manyforge/assistant",
            json=provider_request("request.2", before_draft, before_workspace),
        ).status_code == 202
        stale = terminal(client, "request.2")

    assert responses[2][0] == 409
    assert responses[2][1]["detail"]["code"] == "composer_workspace_stale"
    assert (stale.get("response") or {}).get("draftMutated") is not True, stale
    assert state.draft_session.snapshot() == after


def test_scene_inspect_callback_returns_the_draft_scene(monkeypatch) -> None:
    node_environment = _node_environment()
    context = _assistant_context(node_environment, "scene_inspect")
    state = ComposerState(
        node_environment=build_node_environment(),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    draft = state.draft_session.snapshot()
    draft["program_loaded"] = True
    draft["authoring"] = {"schema_version": "manyforge.composer_authoring_forest.v2",
                          "forest": [], "scene": [_resource("draft_only")]}
    monkeypatch.setattr(state.draft_session, "snapshot", lambda: deepcopy(draft))
    from manyforge_composer.backend import routes_assistant_draft as route
    monkeypatch.setattr(route, "read_active_assistant_context", lambda _state, *, mode: context)
    body = _body(state, node_environment, context.source_model, context.assistant_mode)
    body["arguments"] = {}
    response = TestClient(create_app(state)).post(
        "/api/assistant/bridge/tools/scene_inspect", json=body)
    assert response.status_code == 200
    result = response.json()
    assert result["mutated"] is False
    assert result["result"]["details"] == {"scene": [_resource("draft_only")]}


def _callback_case(tool_id: str, session) -> dict:
    """Valid arguments for each tool whose named type uses local references."""

    scene = session.snapshot()["authoring"]["scene"][0]
    return {
        # node: a real motion node, shape-valid (its target is a Draft diagnostic).
        "tree_draft_insert_node": {"parent_instance_id": "@root", "node": {
            "node_id": "motion.move_to_pose", "instance_id": "assistant_approach",
            "parameters": {"velocity_scale": 0.3}, "children": []}},
        "blackboard_draft_upsert_keys": {"keys": [{
            "key": "scratch.flag", "type": "bool", "access": "read_write",
            "persistence": "run"}]},
        "program_draft_upsert_parameters": {"parameters": [{
            "parameter_id": "assistant_speed", "type": "float64", "default": 0.5,
            "constraints": {}}]},
        "scene_draft_update_object": {"scene_resource": {
            "scene_resource_id": scene["scene_resource_id"], "shape": scene["shape"],
            "pose": scene["pose"]}},
    }[tool_id]


@pytest.mark.parametrize("tool_id", [
    "tree_draft_insert_node", "blackboard_draft_upsert_keys",
    "program_draft_upsert_parameters", "scene_draft_update_object",
])
def test_typed_arguments_with_local_references_validate_in_composer(
    monkeypatch, tool_id: str
) -> None:
    node_environment = _node_environment()
    environment = build_node_environment()
    from manyforge_headless_api.service import _assistant_instruments

    context = _assistant_context(
        node_environment, tool_id, [entry.id for entry in _assistant_instruments().entries()])
    state = ComposerState(
        node_environment=environment,
        program=Program(_PROGRAM_PATH, environment.catalog),
        program_source_path=str(_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    if tool_id == "blackboard_draft_upsert_keys":
        state.draft_session.mutate(
            tool_id="blackboard_draft_upsert_namespaces",
            arguments={"namespaces": [{"namespace_id": "scratch", "owner": "program"}]},
            expected_revision=state.draft_session.revision,
            catalog=environment.catalog,
        )
    from manyforge_composer.backend import routes_assistant_draft as route
    monkeypatch.setattr(route, "read_active_assistant_context", lambda _state, *, mode: context)
    body = _body(state, node_environment, context.source_model, context.assistant_mode)
    body["arguments"] = _callback_case(tool_id, state.draft_session)
    response = TestClient(create_app(state)).post(
        f"/api/assistant/bridge/tools/{tool_id}", json=body)
    assert response.status_code == 200
    result = response.json()
    assert result["success"] is True, result["result"]
    assert result["draftMutated"] is True


def _grasp_policy(policy_id: str = "assistant_grasp") -> dict:
    return {
        "api_version": "manyforge.contact_policy.v1",
        "policy_id": policy_id,
        "mode": "contact_grasp",
        "sensing_mode": "geometry_only",
        "default_unmatched_response": "hard_stop",
        "zones": [{
            "zone_id": "fingertips",
            "body_pairs": [{
                "body_a": {"kind": "motion_endpoint"},
                "body_b": {"kind": "runtime_object", "runtime_object_id": "pick_target"},
            }],
            "max_penetration_m": 0.002,
            "terminal_window_start_fraction": 0.8,
            "violation_response": "hard_stop",
        }],
    }


def _contact_policy_callback(monkeypatch, state, tool_id: str, arguments: dict) -> dict:
    node_environment = _node_environment()
    context = _assistant_context(node_environment, tool_id)
    from manyforge_composer.backend import routes_assistant_draft as route
    monkeypatch.setattr(route, "read_active_assistant_context", lambda _state, *, mode: context)
    body = _body(state, node_environment, context.source_model, context.assistant_mode)
    body["arguments"] = arguments
    response = TestClient(create_app(state)).post(
        f"/api/assistant/bridge/tools/{tool_id}", json=body)
    assert response.status_code == 200
    return response.json()


def _contact_policy_state() -> ComposerState:
    environment = build_node_environment()
    return ComposerState(
        node_environment=environment,
        program=Program(_PROGRAM_PATH, environment.catalog),
        program_source_path=str(_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )


def test_contact_policy_callbacks_upsert_then_remove(monkeypatch) -> None:
    state = _contact_policy_state()
    upserted = _contact_policy_callback(
        monkeypatch, state, "program_draft_upsert_contact_policies",
        {"contact_policies": [_grasp_policy()]})
    assert upserted["success"] is True, upserted["result"]
    assert upserted["draftMutated"] is True
    rows = state.draft_session.snapshot()["authoring"]["contact_policies"]
    assert [row["policy_id"] for row in rows] == ["assistant_grasp"]
    # Composer computes the version.
    assert rows[0]["version"].startswith("sha256:")

    removed = _contact_policy_callback(
        monkeypatch, state, "program_draft_remove_contact_policies",
        {"contact_policy_ids": ["assistant_grasp"]})
    assert removed["success"] is True, removed["result"]
    assert removed["draftMutated"] is True
    assert state.draft_session.snapshot()["authoring"].get("contact_policies", []) == []


def test_contact_policy_callback_refuses_invalid_policy_with_typed_error(monkeypatch) -> None:
    state = _contact_policy_state()
    policy = _grasp_policy()
    # Shape-valid, but two motion_endpoint selectors: no runtime object.
    policy["zones"][0]["body_pairs"][0]["body_b"] = {"kind": "motion_endpoint"}
    before = deepcopy(state.draft_session.snapshot())
    refused = _contact_policy_callback(
        monkeypatch, state, "program_draft_upsert_contact_policies",
        {"contact_policies": [policy]})
    assert refused["success"] is False
    assert refused["draftMutated"] is False
    assert refused["result"]["kind"] == "validation_error"
    assert refused["result"]["httpStatus"] == 400
    assert refused["result"]["message"] == (
        "contact pair must join one component body or motion endpoint to one runtime object"
    )
    # The Draft session's typed code travels with the result; text stays presentation.
    assert refused["result"]["detail"] == {
        "code": "draft_arguments_invalid",
        "message": refused["result"]["message"],
    }
    assert state.draft_session.snapshot() == before


def _setup_insert(monkeypatch):
    node_environment = _node_environment()
    environment = build_node_environment()
    from manyforge_headless_api.service import _assistant_instruments

    context = _assistant_context(node_environment, "tree_draft_insert_node",
                                 [entry.id for entry in _assistant_instruments().entries()])
    state = ComposerState(
        node_environment=environment,
        program=Program(_PROGRAM_PATH, environment.catalog),
        program_source_path=str(_PROGRAM_PATH),
        headless_client=_AdmissionHeadless(),  # type: ignore[arg-type]
    )
    from manyforge_composer.backend import routes_assistant_draft as route
    monkeypatch.setattr(route, "read_active_assistant_context", lambda _state, *, mode: context)
    body = _body(state, node_environment, context.source_model, context.assistant_mode)
    body["arguments"] = {"parent_instance_id": "@root", "node": {
        "node_id": "scene.attach_carried_object", "instance_id": "assistant_attach",
        "parameters": {"target": {"component_id": "ghost_robot", "command_target_id": "gripper"}},
        "children": []}}
    return state, body


def test_committed_edit_reports_reference_diagnostics_of_the_installed_draft(monkeypatch) -> None:
    from manyforge_composer.backend import routes_draft

    state, body = _setup_insert(monkeypatch)
    calls = []

    def validate(actual_state, authoring, *, expected_identity):
        # The validator sees the installed authoring at the post-commit identity.
        calls.append(expected_identity)
        assert expected_identity == actual_state.authoring_workspace.identity()
        text = json.dumps(authoring)
        rows = [{"code": "component_reference_missing",
                 "location": {"document": "program", "path": "/tree/children/0/parameters/target",
                              "node_instance_id": "assistant_attach", "parameter": "target"},
                 "payload": {"component_id": "ghost_robot"}}] if "ghost_robot" in text else []
        return {"reference_validation": {"selector_facts": {}, "diagnostics": rows}}

    monkeypatch.setattr(routes_draft, "validate_captured_program_source", validate)
    response = TestClient(create_app(state)).post(
        "/api/assistant/bridge/tools/tree_draft_insert_node", json=body)
    assert response.status_code == 200
    result = response.json()
    assert result["draftMutated"] is True, result["result"]
    assert [row["code"] for row in result["result"]["referenceDiagnostics"]] == ["component_reference_missing"]
    assert len(calls) == 1


def test_committed_edit_reports_unavailable_reference_validation_as_one_row(monkeypatch) -> None:
    state, body = _setup_insert(monkeypatch)  # no Deployment Draft source root
    response = TestClient(create_app(state)).post(
        "/api/assistant/bridge/tools/tree_draft_insert_node", json=body)
    assert response.status_code == 200
    result = response.json()
    assert result["draftMutated"] is True, result["result"]
    rows = result["result"]["referenceDiagnostics"]
    assert [row["code"] for row in rows] == ["deployment_draft_unavailable"]



def test_diagnostic_paths_resolve_to_the_node_they_point_into() -> None:
    from manyforge_composer.backend.routes_assistant_draft import _with_node

    forest = [{"instance_id": "pick_and_place", "node_id": "sequence", "children": [
        {"instance_id": "a", "node_id": "timer", "children": []},
        {"instance_id": "b", "node_id": "timer", "children": []},
        {"instance_id": "retry_pick", "node_id": "retry", "children": [
            {"instance_id": "move_to_pick", "node_id": "motion.move_to_pose", "children": []}]}]}]
    nested = _with_node({"path": "forest[0].children[2].children[0].value_sources.velocity_scale"},
                        forest)
    assert nested["node"] == {"instance_id": "move_to_pick", "node_id": "motion.move_to_pose",
                              "path": "value_sources.velocity_scale"}
    pointer = _with_node({"location": {"path": "/forest/0/children/1/parameters/target"}}, forest)
    assert pointer["node"] == {"instance_id": "b", "node_id": "timer", "path": "parameters.target"}
    assert _with_node({"path": "forest[0]"}, forest)["node"]["instance_id"] == "pick_and_place"
    assert "node" not in _with_node({"path": "forest"}, forest)
    assert "node" not in _with_node({"path": "forest[0].children[9]"}, forest)


# OD-93: the last verified context is kept by its Headless identity (ETag).


def _context_headless(reads: dict, *, epoch: str = "headless.epoch.1"):
    """A real Headless app (stub service) behind the real HTTP client."""

    from types import SimpleNamespace

    from manyforge_headless_api.app import create_app as create_headless_app
    from manyforge_headless_api.client import HeadlessHttpClient

    def source(digest: str) -> dict:
        return {
            "available": True,
            "freshness": "fresh",
            "known_gaps": [],
            "projection_owner_epoch": epoch,
            "source_content_digest": digest,
        }

    def context(_mode: str) -> dict:
        reads["count"] += 1
        return {
            "assistant_mode": {"source": source(reads["mode_digest"])},
            "node_environment": {"source": source("sha256:" + "2" * 64)},
            "schema_version": "manyforge.assistant_mode_context.v1",
            "source_model": {"source": source("sha256:" + "3" * 64)},
        }

    app = create_headless_app(
        service=SimpleNamespace(assistant_mode_context=context)  # type: ignore[arg-type]
    )
    return HeadlessHttpClient("http://testserver", client=TestClient(app))


def _counted_context_verification(monkeypatch) -> dict:
    from types import SimpleNamespace

    from manyforge_composer.backend import assistant_mode_client

    counts = {"verify": 0, "resolve": 0}

    def verify(_state, payload, *, mode):
        counts["verify"] += 1
        return SimpleNamespace(
            assistant_mode=SimpleNamespace(payload=payload["assistant_mode"]),
        )

    def resolve(mode_projection, *, requested_tools, field_policy):
        counts["resolve"] += 1
        return {"catalogHash": mode_projection["source"]["source_content_digest"]}

    monkeypatch.setattr(assistant_mode_client, "verify_active_assistant_context", verify)
    monkeypatch.setattr(assistant_mode_client, "resolve_assistant_catalog", resolve)
    return counts


def test_unchanged_identity_reuses_the_verified_context(monkeypatch) -> None:
    from types import SimpleNamespace

    from manyforge_composer.backend.assistant_mode_client import (
        AssistantContextCache,
        read_active_assistant_context,
    )

    counts = _counted_context_verification(monkeypatch)
    reads = {"count": 0, "mode_digest": "sha256:" + "1" * 64}
    state = SimpleNamespace(
        headless_client=_context_headless(reads),
        assistant_context_cache=AssistantContextCache(),
    )
    contexts = [
        read_active_assistant_context(state, mode="composer-assistant")
        for _ in range(3)
    ]
    catalogs = [
        state.assistant_context_cache.resolved_catalog(
            context, requested_tools=["program_read"], field_policy={}
        )
        for context in contexts
    ]
    assert reads["count"] == 3
    assert counts == {"verify": 1, "resolve": 1}
    assert all(context is contexts[0] for context in contexts)
    assert all(catalog is catalogs[0] for catalog in catalogs)
    # A different request shape resolves again from the same verification.
    state.assistant_context_cache.resolved_catalog(
        contexts[0], requested_tools=[], field_policy={}
    )
    assert counts == {"verify": 1, "resolve": 2}


def test_changed_identity_and_restart_are_handled(monkeypatch) -> None:
    from types import SimpleNamespace

    from manyforge_composer.backend.assistant_mode_client import (
        AssistantContextCache,
        read_active_assistant_context,
    )

    counts = _counted_context_verification(monkeypatch)
    reads = {"count": 0, "mode_digest": "sha256:" + "1" * 64}
    state = SimpleNamespace(
        headless_client=_context_headless(reads),
        assistant_context_cache=AssistantContextCache(),
    )
    first = read_active_assistant_context(state, mode="composer-assistant")

    # A restarted Headless serving the same content: 304, no re-verify.
    state.headless_client = _context_headless(reads, epoch="headless.epoch.2")
    assert read_active_assistant_context(state, mode="composer-assistant") is first
    assert counts["verify"] == 1

    # An Apply changes the content: full verify of the new context.
    reads["mode_digest"] = "sha256:" + "4" * 64
    changed = read_active_assistant_context(state, mode="composer-assistant")
    assert counts["verify"] == 2
    assert changed.assistant_mode.payload["source"]["source_content_digest"] == (
        "sha256:" + "4" * 64
    )
    assert state.assistant_context_cache.resolved_catalog(
        changed, requested_tools=[], field_policy={}
    ) == {"catalogHash": "sha256:" + "4" * 64}


def test_context_read_failures_keep_their_typed_codes() -> None:
    from types import SimpleNamespace

    from manyforge_composer.backend.assistant_mode_client import (
        ActiveAssistantModeProjectionError,
        AssistantContextCache,
        read_active_assistant_context,
    )
    from manyforge_headless_api.client import HeadlessClientError

    class Unavailable:
        def assistant_mode_context_if_changed(self, mode, etag):
            raise HeadlessClientError(
                "assistant_mode_projection_unavailable", "down", 503
            )

    state = SimpleNamespace(
        headless_client=Unavailable(),
        assistant_context_cache=AssistantContextCache(),
    )
    with pytest.raises(ActiveAssistantModeProjectionError) as error:
        read_active_assistant_context(state, mode="composer-assistant")
    assert error.value.code == "assistant_mode_projection_unavailable"
    assert error.value.status_code == 503


def test_model_sampling_is_optional_bounded_and_kept() -> None:
    contract = {
        "default_timeout_s": 30,
        "max_output_tokens": 512,
        "supported_image_transports": [],
        "supports_image_input": False,
        "supports_json_object": True,
        "supports_response_tools": True,
        "tool_call_family": "openai_chat_tools",
    }
    assert "sampling" not in _model_request_contract(dict(contract))
    sampling = {"temperature": 0.7, "top_k": 20, "top_p": 0.8}
    assert _model_request_contract({**contract, "sampling": sampling})["sampling"] == sampling
    for bad in ({}, {"min_p": 0.1}, {"temperature": 2.5}, {"temperature": -0.1},
                {"top_p": 0}, {"top_p": 1.5}, {"top_k": 0}, {"top_k": 1.5}):
        with pytest.raises(ActiveAssistantModeProjectionError):
            _model_request_contract({**contract, "sampling": bad})
