"""Reference-validation capture and stale-response fences."""

from copy import deepcopy
from types import SimpleNamespace

from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
from manyforge_composer.backend import routes_draft
from manyforge_headless_api.client import HeadlessClientError


class Workspace:
    def __init__(self):
        self.deployment = {"components": []}
        self.generation = SimpleNamespace(
            workspace_epoch="epoch", workspace_revision=1,
            program_draft_id="program", program_draft_revision=2,
            deployment_draft_id="deployment", deployment_draft_revision=3,
        )

    def identity(self):
        return self.generation

    def snapshot(self):
        return SimpleNamespace(identity=self.generation,
            as_dict=lambda: {"deployment": {"document": deepcopy(self.deployment)}})


def _closure(deployment):
    return {"schema_version": "closure", "components": deepcopy(deployment["components"])}


def _client(monkeypatch, *, stale=False, mismatch=False):
    workspace = Workspace()
    captured = []
    class Headless:
        # Mirrors the capacity-one prepared candidate: a digest-only request
        # hits only the candidate the last full request prepared.
        held = None
        def request(self, method, path, *, body):
            captured.append(deepcopy(body))
            key = (raw_sha256_digest(canonical_json_bytes(body["deployment"])),
                   body.get("model_source_closure_digest"))
            if "model_source_closure" in body:
                key = (key[0], raw_sha256_digest(canonical_json_bytes(body["model_source_closure"])))
                Headless.held = key
            elif Headless.held != key:
                raise HeadlessClientError("model_sources_unknown", "unknown", 409)
            response = {"schema_version": "manyforge.program_reference_validation_response.v2",
                        "workspace_generation": deepcopy(body["workspace_generation"]),
                        "program_content_digest": body["program_content_digest"],
                        "diagnostics": [], "total_count": 0, "truncated": False,
                        "selector_facts": {key: [] for key in ("components", "targets", "signals", "frames", "bodies", "rules")}}
            if stale:
                workspace.generation = SimpleNamespace(**{**vars(workspace.generation), "workspace_revision": 2})
            if mismatch:
                response["program_content_digest"] = "sha256:" + "0" * 64
            return response
    state = SimpleNamespace(authoring_workspace=workspace, deployment_source_project_path="/tmp",
                            headless_client=Headless(), node_catalog=object())
    monkeypatch.setattr(routes_draft, "capture_deployment_model_sources", lambda deployment, **_kwargs: _closure(deployment))
    monkeypatch.setattr(routes_draft, "derive_candidate", lambda *_args: (None, []))
    app = FastAPI()
    app.include_router(routes_draft.create_draft_router(state))
    client = TestClient(app)
    client.workspace = workspace
    return client, captured


def test_reference_validation_echoes_detached_generation_and_digest(monkeypatch):
    client, captured = _client(monkeypatch)
    response = client.post("/api/program/validate", json={"authoring": {"forest": []}})
    assert response.status_code == 200
    reference = response.json()["reference_validation"]
    assert reference["total_count"] == 0 and not reference["truncated"]
    assert reference["workspace_generation"]["deployment_draft_revision"] == 3
    assert reference["program_content_digest"] == captured[0]["program_content_digest"]
    assert captured[0]["program"] == {"forest": []}


def test_reference_validation_sends_digest_first_and_full_closure_once_on_unknown(monkeypatch):
    client, captured = _client(monkeypatch)
    digest = raw_sha256_digest(canonical_json_bytes(_closure({"components": []})))
    assert client.post("/api/program/validate", json={"authoring": {"forest": []}}).status_code == 200
    assert captured[0]["schema_version"] == "manyforge.program_reference_validation_request.v3"
    assert captured[0]["model_source_closure_digest"] == digest
    assert "model_source_closure" not in captured[0]
    assert captured[1]["model_source_closure"] == _closure({"components": []})
    assert "model_source_closure_digest" not in captured[1]
    assert len(captured) == 2
    # The candidate is now held: the next validation needs no resend.
    assert client.post("/api/program/validate", json={"authoring": {"forest": [1]}}).status_code == 200
    assert len(captured) == 3 and captured[2]["model_source_closure_digest"] == digest


def test_reference_validation_resends_full_closure_after_deployment_change(monkeypatch):
    client, captured = _client(monkeypatch)
    assert client.post("/api/program/validate", json={"authoring": {"forest": []}}).status_code == 200
    client.workspace.deployment = {"components": ["robot"]}
    assert client.post("/api/program/validate", json={"authoring": {"forest": []}}).status_code == 200
    assert len(captured) == 4
    assert captured[2]["model_source_closure_digest"] != captured[0]["model_source_closure_digest"]
    assert captured[2]["model_source_closure_digest"] == raw_sha256_digest(
        canonical_json_bytes(_closure({"components": ["robot"]})))
    assert captured[3]["model_source_closure"] == _closure({"components": ["robot"]})


def test_reference_validation_rejects_stale_and_mismatched_replies(monkeypatch):
    for options, expected in (({"stale": True}, 409), ({"mismatch": True}, 502)):
        client, _captured = _client(monkeypatch, **options)
        response = client.post("/api/program/validate", json={"authoring": {"forest": []}})
        assert response.status_code == expected
