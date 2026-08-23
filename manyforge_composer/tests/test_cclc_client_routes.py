"""Composer remains a stateless proxy for the open CCLC surface."""

from __future__ import annotations

import json
from typing import Any

from fastapi.testclient import TestClient
import httpx

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessHttpClient


def _lifecycle_body() -> dict[str, Any]:
    return {
        "request_context": {
            "request_id": "request.lifecycle.1",
            "request_owner_epoch": "composer.epoch.1",
            "source_label": "composer-ui",
            "correlation_id": None,
        },
        "controllable_component_id": "component.arm",
        "intent": "prepare",
        "expected_graph_revision": "graph.1",
        "expected_component_lifecycle_generation": 4,
        "expected_control_domains": [
            {
                "control_domain_id": "domain.arm",
                "control_domain_revision": "domain.revision.1",
                "controller_topology_id": "topology.arm",
                "topology_revision": "topology.revision.1",
                "expected_active_control_configuration": {
                    "presence": "absent",
                    "active_control_configuration_id": None,
                    "active_control_configuration_generation": None,
                },
                "expected_control_bindings": [
                    {
                        "control_binding_id": "binding.arm",
                        "control_binding_revision": "binding.revision.1",
                        "control_route_id": "route.arm",
                    }
                ],
            }
        ],
        "reason": "operator-confirmed",
        "confirmed": True,
        "expected_admission_generation": 7,
    }


def test_composer_proxies_exact_component_and_lifecycle_requests() -> None:
    forwarded: list[tuple[str, str, dict[str, Any] | None]] = []

    def handler(request: httpx.Request) -> httpx.Response:
        body = json.loads(request.content) if request.content else None
        forwarded.append((request.method, request.url.path, body))
        if request.url.path.endswith("/route-templates"):
            payload = {
                "controllable_component_id": "component.arm",
                "graph_revision": "graph.1",
                "retained_bundle_id": "bundle.1",
                "templates": [],
            }
        elif request.method == "GET":
            payload = {
                "controllable_component_id": "component.arm",
                "component_lifecycle_generation": 4,
            }
        else:
            payload = {
                "acceptance": None,
                "replay_status": "request_id_conflict",
                "status": None,
            }
        return httpx.Response(200, json=payload, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    headless = HeadlessHttpClient("http://unused.test", client=raw)
    client = TestClient(create_app(ComposerState(headless_client=headless)))
    try:
        snapshot = client.get("/api/cclc/components/component.arm")
        templates = client.get(
            "/api/cclc/components/component.arm/route-templates"
        )
        accepted = client.post(
            "/api/cclc/lifecycle/requests", json=_lifecycle_body()
        )
    finally:
        raw.close()

    assert snapshot.status_code == 200
    assert snapshot.json()["component_lifecycle_generation"] == 4
    assert templates.status_code == 200
    assert templates.json()["templates"] == []
    assert accepted.status_code == 200
    assert accepted.json()["replay_status"] == "request_id_conflict"
    cclc_forwarded = [row for row in forwarded if "/api/cclc/" in row[1]]
    assert cclc_forwarded == [
        ("GET", "/api/cclc/components/component.arm", None),
        (
            "GET",
            "/api/cclc/components/component.arm/route-templates",
            None,
        ),
        (
            "POST",
            "/api/cclc/lifecycle/requests",
            _lifecycle_body(),
        ),
    ]
    assert not hasattr(client.app.state, "lifecycle_owner")
