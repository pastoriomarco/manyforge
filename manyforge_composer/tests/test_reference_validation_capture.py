"""Reference-validation capture and stale-response fences."""

from copy import deepcopy
from types import SimpleNamespace

from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend import routes_draft


class Workspace:
    def __init__(self):
        self.generation = SimpleNamespace(
            workspace_epoch="epoch", workspace_revision=1,
            program_draft_id="program", program_draft_revision=2,
            deployment_draft_id="deployment", deployment_draft_revision=3,
        )

    def identity(self):
        return self.generation

    def snapshot(self):
        return SimpleNamespace(identity=self.generation,
            as_dict=lambda: {"deployment": {"document": {"components": []}}})


def _client(monkeypatch, *, stale=False, mismatch=False):
    workspace = Workspace()
    captured = []
    class Headless:
        def request(self, method, path, *, body):
            captured.append(deepcopy(body))
            response = {"schema_version": "manyforge.program_reference_validation_response.v2",
                        "workspace_generation": deepcopy(body["workspace_generation"]),
                        "program_content_digest": body["program_content_digest"],
                        "diagnostics": [], "total_count": 0, "truncated": False,
                        "selector_facts": {key: [] for key in ("components", "targets", "signals", "frames", "bodies")}}
            if stale:
                workspace.generation = SimpleNamespace(**{**vars(workspace.generation), "workspace_revision": 2})
            if mismatch:
                response["program_content_digest"] = "sha256:" + "0" * 64
            return response
    state = SimpleNamespace(authoring_workspace=workspace, deployment_source_project_path="/tmp",
                            headless_client=Headless(), node_catalog=object())
    monkeypatch.setattr(routes_draft, "capture_deployment_model_sources", lambda *_args, **_kwargs: {"schema_version": "closure", "components": []})
    monkeypatch.setattr(routes_draft, "derive_candidate", lambda *_args: (None, []))
    app = FastAPI()
    app.include_router(routes_draft.create_draft_router(state))
    return TestClient(app), captured


def test_reference_validation_echoes_detached_generation_and_digest(monkeypatch):
    client, captured = _client(monkeypatch)
    response = client.post("/api/program/validate", json={"authoring": {"forest": []}})
    assert response.status_code == 200
    reference = response.json()["reference_validation"]
    assert reference["total_count"] == 0 and not reference["truncated"]
    assert reference["workspace_generation"]["deployment_draft_revision"] == 3
    assert reference["program_content_digest"] == captured[0]["program_content_digest"]
    assert captured[0]["program"] == {"forest": []}


def test_reference_validation_rejects_stale_and_mismatched_replies(monkeypatch):
    for options, expected in (({"stale": True}, 409), ({"mismatch": True}, 502)):
        client, _captured = _client(monkeypatch, **options)
        response = client.post("/api/program/validate", json={"authoring": {"forest": []}})
        assert response.status_code == expected
