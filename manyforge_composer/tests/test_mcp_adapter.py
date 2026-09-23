"""Real stateless MCP/ASGI boundary; no robot or alternate lifecycle owner."""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
import threading

import httpx
import anyio
from fastapi import FastAPI
from fastapi.testclient import TestClient
import jsonschema
import pytest

from manyforge_composer.backend.app import create_app
from manyforge_composer.backend.authoring_forest import empty_authoring
from manyforge_composer.backend.mcp_adapter import (
    ComposerMcpAdapter,
    TOOL_ROUTES,
    tool_contracts,
)
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessHttpClient


def _security(tmp_path: Path) -> SecurityConfig:
    return SecurityConfig(allowed_fs_roots=[str(tmp_path)], mcp_allowed_hosts=["testserver"])


def _mcp_state(headless) -> ComposerState:
    """Build a transport-focused state without exercising authoring startup."""

    state = ComposerState(headless_client=headless)
    state.authoring_admission = None
    return state


def _call(client: TestClient, name: str, arguments: dict, *, request_id: int = 1):
    return client.post(
        "/mcp/",
        headers={"Accept": "application/json, text/event-stream"},
        json={
            "jsonrpc": "2.0",
            "id": request_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        },
    )


def test_optional_off_does_not_construct_mcp(tmp_path, monkeypatch):
    def forbidden(*args, **kwargs):
        raise AssertionError("MCP constructed while disabled")

    monkeypatch.setattr(ComposerMcpAdapter, "__init__", forbidden)
    with TestClient(create_app(ComposerState(), security=_security(tmp_path))) as client:
        assert client.post("/mcp/").status_code in {404, 405}
        assert not any(getattr(route, "name", "") == "mcp" for route in client.app.routes)
        assert client.get("/api/program/draft").status_code == 200


def test_all_19_tools_derive_action_schemas_from_real_routes(tmp_path):
    state = _mcp_state(object())
    app = create_app(state, security=_security(tmp_path))
    contracts = tool_contracts(app)
    assert len(contracts) == len(TOOL_ROUTES) == 19
    api = app.openapi()
    for tool in contracts:
        jsonschema.Draft202012Validator.check_schema(tool["inputSchema"])
        actions = TOOL_ROUTES[tool["name"]]
        assert len(tool["inputSchema"]["oneOf"]) == len(actions)
        for variant in tool["inputSchema"]["oneOf"]:
            action = variant["properties"]["action"]["const"]
            method, path = actions[action]
            assert method.lower() in api["paths"][path]
    validator = next(t for t in contracts if t["name"] == "manyforge_program_validate")
    assert validator["annotations"]["readOnlyHint"]
    assert next(t for t in contracts if t["name"] == "manyforge_skills")["annotations"][
        "readOnlyHint"
    ]
    assert not next(t for t in contracts if t["name"] == "manyforge_kernel")["annotations"][
        "readOnlyHint"
    ]


def test_real_mcp_skill_expansion_uses_existing_headless_http_client(tmp_path):
    calls = []
    body = {
        "schema_version": "manyforge.skill_expansion_request.v1",
        "expected_environment_digest": "sha256:" + "1" * 64,
        "skill_id": "move_to_named_config",
        "instance_prefix": "selected",
        "arguments": {},
        "component_roles": {},
        "target_roles": {},
        "program_context": {},
    }
    owner_result = {
        "schema_version": "manyforge.skill_expansion.v1",
        "tree": {},
        "trace": [],
    }

    def owner(request):
        calls.append((request.method, request.url.path, json.loads(request.content)))
        if request.url.path != "/api/skills/expand":
            return httpx.Response(
                503,
                json={
                    "detail": {
                        "code": "fixture_owner_unavailable",
                        "message": "No effect owner in this transport test",
                    }
                },
            )
        return httpx.Response(200, json=owner_result)

    with httpx.Client(
        base_url="http://headless.test", transport=httpx.MockTransport(owner)
    ) as transport:
        headless = HeadlessHttpClient("http://headless.test", client=transport)
        state = _mcp_state(headless)
        before = state.draft_session.snapshot()
        with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
            result = _call(client, "manyforge_skills", {"action": "expand", "body": body}).json()[
                "result"
            ]
            assert not result["isError"], result
            assert json.loads(result["content"][0]["text"]) == owner_result
            assert calls == [("POST", "/api/skills/expand", body)]
            assert state.draft_session.snapshot() == before


def test_real_mcp_initialize_discover_read_and_pure_validation(tmp_path):
    state = ComposerState()
    before = state.draft_session.snapshot()
    source = empty_authoring(state.node_catalog)
    app = create_app(state, security=_security(tmp_path), enable_mcp=True)
    with TestClient(app) as client:
        initialized = client.post(
            "/mcp/",
            headers={"Accept": "application/json, text/event-stream"},
            json={
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {
                    "protocolVersion": "2025-06-18",
                    "capabilities": {},
                    "clientInfo": {"name": "qualification", "version": "1"},
                },
            },
        )
        assert initialized.status_code == 200
        assert "mcp-session-id" not in initialized.headers
        listed = client.post(
            "/mcp/",
            headers={"Accept": "application/json, text/event-stream"},
            json={
                "jsonrpc": "2.0",
                "id": 2,
                "method": "tools/list",
            },
        )
        assert listed.status_code == 200
        assert any(t["name"] == "manyforge_catalog" for t in listed.json()["result"]["tools"])
        response = _call(client, "manyforge_program_tree", {"action": "read"}).json()["result"]
        assert not response["isError"]
        assert json.loads(response["content"][0]["text"]) == before
        validated = _call(
            client,
            "manyforge_program_validate",
            {
                "action": "source",
                "body": {"authoring": source},
            },
        ).json()["result"]
        assert not validated["isError"]
        result = json.loads(validated["content"][0]["text"])
        assert result["valid"] is False and result["diagnostics"]
        assert state.draft_session.snapshot() == before
        bad = _call(
            client,
            "manyforge_program_validate",
            {
                "action": "source",
                "body": {"authoring": {}},
            },
        ).json()["result"]
        assert bad["isError"]
        assert state.draft_session.snapshot() == before


@pytest.mark.parametrize(
    "arguments",
    [
        {"action": "entry", "path": {"entry_id": "../program/draft"}},
        {"action": "entry", "path": {"entry_id": "%2e%2e"}},
        {"action": "entry", "path": {"entry_id": "repeat?other=yes"}},
        {"action": "list", "path": {"entry_id": "repeat"}},
        {"action": "list", "body": {}},
        {"action": "list", "url": "http://outside.test"},
        {"action": "missing"},
    ],
)
def test_closed_routing_cannot_be_used_as_a_generic_http_proxy(tmp_path, arguments):
    with TestClient(
        create_app(ComposerState(), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        result = _call(client, "manyforge_catalog", arguments).json()["result"]
        assert result["isError"]


def test_mcp_keeps_host_origin_and_request_size_bounds(tmp_path):
    with TestClient(
        create_app(ComposerState(), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        assert client.post("/mcp/", headers={"Host": "foreign.invalid"}, json={}).status_code == 421
        assert (
            client.post("/mcp/", headers={"Origin": "http://foreign.invalid"}, json={}).status_code
            == 403
        )
        assert client.post("/mcp/", content=b"x" * (1024 * 1024 + 1)).status_code == 413


@pytest.mark.parametrize("body", [{}, [], "malformed", True, None])
def test_runtime_failure_and_malformed_stop_keep_real_route_semantics(tmp_path, body):
    class Owner:
        def __init__(self):
            self.stops = []

        def recognized_malformed_stop(self, path):
            self.stops.append(path)
            return {"accepted": True, "reason": "recognized_malformed_stop"}

    owner = Owner()
    state = _mcp_state(owner)
    with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
        invalid = _call(
            client, "manyforge_cycle_control", {"action": "command", "body": {}}
        ).json()["result"]
        assert invalid["isError"]
        stop = _call(client, "manyforge_kernel", {"action": "full_stop", "body": body}).json()[
            "result"
        ]
        assert not stop["isError"]
        assert json.loads(stop["content"][0]["text"])["accepted"]
        assert owner.stops == ["/api/execution/full-stop"]


def test_adapter_keeps_concurrency_and_never_retries_an_effect(tmp_path):
    # The ordinary route is deliberately held. The Stop route must still
    # enter; MCP must add neither a shared lock nor an execution queue.
    app = FastAPI()
    entered, release = threading.Event(), threading.Event()
    calls = []

    @app.post("/api/execution/operations")
    def ordinary():
        calls.append("ordinary")
        entered.set()
        assert release.wait(5)
        return {"accepted": True}

    @app.post("/api/execution/full-stop")
    def stop():
        calls.append("stop")
        return {"accepted": True}

    adapter = ComposerMcpAdapter(app, _security(tmp_path))

    async def exercise():
        pending = asyncio.create_task(
            adapter.invoke("manyforge_kernel", {"action": "submit", "body": {}})
        )
        assert await asyncio.to_thread(entered.wait, 5)
        try:
            response = await asyncio.wait_for(
                adapter.invoke("manyforge_kernel", {"action": "full_stop", "body": {}}),
                1,
            )
            assert response.json()["accepted"]
        finally:
            release.set()
        assert (await pending).json()["accepted"]

    asyncio.run(exercise())
    assert calls == ["ordinary", "stop"]


def test_validation_endpoint_is_also_available_without_mcp(tmp_path):
    state = ComposerState()
    with TestClient(create_app(state, security=_security(tmp_path))) as client:
        before = client.get("/api/program/draft").json()
        response = client.post(
            "/api/program/validate",
            json={"authoring": empty_authoring(state.node_catalog)},
        )
        assert response.status_code == 200 and response.json()["diagnostics"]
        assert client.get("/api/program/draft").json() == before


def test_real_mcp_stop_bypasses_a_saturated_ordinary_worker_pool(tmp_path):
    class Owner:
        def recognized_malformed_stop(self, path):
            return {"accepted": True, "path": path}

    app = create_app(
        _mcp_state(Owner()),
        security=_security(tmp_path),
        enable_mcp=True,
    )
    entered, release = threading.Event(), threading.Event()

    def held():
        entered.set()
        assert release.wait(5)

    async def exercise():
        limiter = anyio.to_thread.current_default_thread_limiter()
        previous = limiter.total_tokens
        limiter.total_tokens = 1
        async with app.router.lifespan_context(app):
            blocker = asyncio.create_task(anyio.to_thread.run_sync(held))
            assert await asyncio.to_thread(entered.wait, 5)
            try:
                async with httpx.AsyncClient(
                    transport=httpx.ASGITransport(app=app), base_url="http://testserver"
                ) as client:
                    response = await asyncio.wait_for(
                        client.post(
                            "/mcp/",
                            headers={
                                "Accept": "application/json, text/event-stream",
                            },
                            json={
                                "jsonrpc": "2.0",
                                "id": 3,
                                "method": "tools/call",
                                "params": {
                                    "name": "manyforge_kernel",
                                    "arguments": {"action": "full_stop", "body": {}},
                                },
                            },
                        ),
                        1,
                    )
                    assert not response.json()["result"]["isError"]
            finally:
                release.set()
                await blocker
                limiter.total_tokens = previous

    asyncio.run(exercise())


@pytest.mark.parametrize("cancel", [True, False])
def test_mounted_requests_retire_on_cancellation_or_send_failure(tmp_path, cancel):
    async def exercise():
        app = FastAPI()
        entered, retired = asyncio.Event(), asyncio.Event()

        @app.post("/api/execution/operations")
        async def ordinary():
            entered.set()
            try:
                if cancel:
                    await asyncio.Event().wait()
                return {"accepted": True}
            finally:
                retired.set()

        adapter = ComposerMcpAdapter(app, _security(tmp_path))
        app.mount("/mcp", adapter.handle_request)
        baseline = set(asyncio.all_tasks())
        payload = json.dumps(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/call",
                "params": {
                    "name": "manyforge_kernel",
                    "arguments": {"action": "submit", "body": {}},
                },
            }
        ).encode()
        scope = {
            "type": "http",
            "asgi": {"version": "3.0"},
            "http_version": "1.1",
            "method": "POST",
            "scheme": "http",
            "path": "/mcp/",
            "raw_path": b"/mcp/",
            "root_path": "",
            "query_string": b"",
            "server": ("testserver", 80),
            "client": ("127.0.0.1", 1234),
            "headers": [
                (b"host", b"testserver"),
                (b"content-type", b"application/json"),
                (b"accept", b"application/json, text/event-stream"),
            ],
        }
        delivered = False

        async def receive():
            nonlocal delivered
            if not delivered:
                delivered = True
                return {"type": "http.request", "body": payload, "more_body": False}
            await asyncio.Event().wait()

        async def send(message):
            if not cancel:
                raise ConnectionError("test transport send failed")

        request = asyncio.create_task(app(scope, receive, send))
        await asyncio.wait_for(entered.wait(), 2)
        if cancel:
            request.cancel()
            with pytest.raises(asyncio.CancelledError):
                await request
        else:
            try:
                await asyncio.wait_for(request, 2)
            except (ConnectionError, ExceptionGroup):
                pass
        assert retired.is_set()
        assert not {task for task in asyncio.all_tasks() - baseline if not task.done()}

    asyncio.run(exercise())


def test_nonfinite_recognized_stop_body_reaches_the_owner(tmp_path):
    class Owner:
        def recognized_malformed_stop(self, path):
            return {"accepted": True, "path": path}

    state = _mcp_state(Owner())
    with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
        response = client.post(
            "/mcp/",
            headers={
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
            },
            content='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"manyforge_kernel","arguments":{"action":"full_stop","body":{"bad":NaN}}}}',
        )
        assert not response.json()["result"]["isError"]


@pytest.mark.parametrize("action", ["stop", "full_stop"])
@pytest.mark.parametrize(
    "body",
    [
        '{"operations":[{"operation_id":"A"}],"operations":[{"operation_id":"B"}]}',
        '{"operations":[{"operation_id":"A","operation_id":"B"}]}',
    ],
)
def test_raw_duplicate_stop_selectors_reach_malformed_containment(tmp_path, action, body):
    class Owner:
        def __init__(self):
            self.stops = []

        def recognized_malformed_stop(self, path):
            self.stops.append(path)
            return {"accepted": True, "malformed": True}

    owner = Owner()
    state = _mcp_state(owner)
    with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
        response = client.post(
            "/mcp/",
            headers={
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
            },
            content='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":'
            '{"name":"manyforge_kernel","arguments":{"action":'
            + json.dumps(action)
            + ',"body":'
            + body
            + "}}}",
        )
        result = response.json()["result"]
        assert not result["isError"]
        assert json.loads(result["content"][0]["text"])["malformed"]
        assert owner.stops == ["/api/execution/" + ("stops" if action == "stop" else "full-stop")]


@pytest.mark.parametrize(
    "params",
    [
        '"name":"manyforge_catalog","name":"manyforge_kernel","arguments":{"action":"stop"}',
        '"name":"manyforge_kernel","arguments":{"action":"submit","action":"stop"}',
        '"name":"manyforge_kernel","arguments":{"action":"stop","body":{},"body":{}}',
        '"name":"manyforge_kernel","arguments":{"action":"stop","path":{"id":"A","id":"B"}}',
        '"name":"manyforge_kernel","arguments":{"action":"submit","body":{"target":"A","target":"B"}}',
    ],
)
def test_raw_duplicate_routing_or_ordinary_bodies_are_rejected(tmp_path, params):
    # A sentinel owner has no effect methods: rejection must precede dispatch.
    state = _mcp_state(object())
    with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
        response = client.post(
            "/mcp/",
            headers={
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
            },
            content='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{' + params + "}}",
        )
        assert response.status_code == 400
        assert response.json()["error"]["code"] == -32600


def test_lossy_duplicate_decoding_must_not_turn_malformed_stop_into_scoped_stop(
    tmp_path,
):
    from manyforge_headless_api.models import StopIntentRequest

    context = {
        "request_id": "stop.1",
        "request_owner_epoch": "owner.1",
        "source_label": "mcp",
    }
    target = lambda identity: {
        "operation_id": identity,
        "expected_operation_revision": 1,
        "expected_stop_generation": 0,
    }
    raw_body = (
        '{"request_context":'
        + json.dumps(context)
        + ',"operations":'
        + json.dumps([target("A")])
        + ',"operations":'
        + json.dumps([target("B")])
        + "}"
    )
    # Pin the counterfactual: the old SDK-last-member-wins path accepts B.
    assert StopIntentRequest.model_validate(json.loads(raw_body)).operations[0].operation_id == "B"

    class Owner:
        def __init__(self):
            self.malformed, self.scoped = 0, 0

        def recognized_malformed_stop(self, path):
            self.malformed += 1
            return {"accepted": True}

        def stop_operations(self, body):
            self.scoped += 1
            return {"accepted": True}

    owner = Owner()
    state = _mcp_state(owner)
    with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
        response = client.post(
            "/mcp/",
            headers={
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
            },
            content='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":'
            '{"name":"manyforge_kernel","arguments":{"action":"stop","body":' + raw_body + "}}}",
        )
        assert not response.json()["result"]["isError"]
        assert (owner.malformed, owner.scoped) == (1, 0)
