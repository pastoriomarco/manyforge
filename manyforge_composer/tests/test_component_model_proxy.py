"""Composer forwards the component model routes (CONTRACTS §1, §2, §5, §7, §10)."""

from typing import Any

from fastapi import FastAPI
from fastapi.testclient import TestClient

from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_headless_api.client import HeadlessClientError


class _Headless:
    def __init__(self) -> None:
        self.calls: list[tuple[Any, ...]] = []

    def components(self):
        self.calls.append(("components",))
        return [{"component_id": "robot", "lifecycle": "enabled"}]

    def component_action(self, component_id, action):
        self.calls.append(("action", component_id, action))
        if action == "reboot":
            raise HeadlessClientError(
                "component_action_unsupported", "robot doesn't offer reboot", 409
            )
        return {"action": action}

    def component_actions(self, action, component_ids):
        self.calls.append(("group", action, component_ids))
        return {"action": action}

    def cell_latch(self):
        self.calls.append(("latch",))
        return {"latched": False, "causes": []}

    def io_signals(self):
        self.calls.append(("io",))
        return []

    def set_io_output(self, signal_id, value):
        self.calls.append(("set", signal_id, value))
        return {"outcome": "succeeded"}

    def program_readiness(self):
        self.calls.append(("readiness",))
        return {"can_execute": True, "can_resume": False, "blocked_reasons": []}


def _app(headless: _Headless) -> TestClient:
    app = FastAPI()
    app.include_router(create_headless_router(headless, object()))  # type: ignore[arg-type]
    return TestClient(app)


def test_component_model_routes_forward_unchanged() -> None:
    headless = _Headless()
    with _app(headless) as client:
        assert client.get("/api/components").json()[0]["component_id"] == "robot"
        action = client.post("/api/components/robot/actions", json={"action": "enable"})
        group = client.post("/api/components/actions", json={"action": "enable"})
        assert group.status_code == 422  # CONTRACTS §2: the caller names the components
        named = client.post(
            "/api/components/actions",
            json={"action": "disable", "component_ids": ["robot"]},
        )
        assert client.get("/api/cell/latch").json() == {"latched": False, "causes": []}
        assert client.get("/api/io").json() == []
        written = client.post("/api/io/robot%2FCO0/set", json={"value": True})
        readiness = client.get("/api/program/readiness")
    assert action.status_code == 202
    assert named.status_code == 202
    assert written.json() == {"outcome": "succeeded"}
    assert readiness.json()["can_execute"] is True
    assert headless.calls == [
        ("components",),
        ("action", "robot", "enable"),
        ("group", "disable", ["robot"]),
        ("latch",),
        ("io",),
        ("set", "robot/CO0", True),
        ("readiness",),
    ]


def test_component_action_refusal_keeps_its_code_and_unknown_actions_are_refused() -> None:
    headless = _Headless()
    with _app(headless) as client:
        refused = client.post("/api/components/robot/actions", json={"action": "reboot"})
        unknown = client.post("/api/components/robot/actions", json={"action": "dance"})
    assert refused.status_code == 409
    assert "component_action_unsupported" in refused.text
    assert unknown.status_code == 422
    assert headless.calls == [("action", "robot", "reboot")]
