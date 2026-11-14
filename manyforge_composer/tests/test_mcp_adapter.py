"""Real stateless MCP/ASGI boundary; no robot or alternate lifecycle owner."""

from __future__ import annotations

import asyncio
import json
import re
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
from manyforge_composer.backend.routes_headless import create_headless_router
from manyforge_composer.backend.security import SecurityConfig
from manyforge_composer.backend.state import ComposerState
from manyforge_headless_api.client import HeadlessHttpClient


def _security(tmp_path: Path) -> SecurityConfig:
    return SecurityConfig(allowed_fs_roots=[str(tmp_path)], mcp_allowed_hosts=["testserver"])


def _mcp_state(headless) -> ComposerState:
    """Build a transport-focused state without exercising authoring startup."""

    state = ComposerState(application_start_id=0x3F9A0000000000A1, headless_client=headless)
    state.authoring_admission = None
    return state


class _StopOwner:
    """Headless double: records each forwarded Stop (path, body) unchanged."""

    def __init__(self):
        self.stops = []

    def request(self, method, path, *, params=(), body=None, timeout_seconds=None):
        assert method == "POST"
        self.stops.append((path, body))
        return {"accepted": True, "forwarded": body}


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
    with TestClient(create_app(ComposerState(application_start_id=0x3F9A0000000000A1), security=_security(tmp_path))) as client:
        assert client.post("/mcp/").status_code in {404, 405}
        assert not any(getattr(route, "name", "") == "mcp" for route in client.app.routes)
        assert client.get("/api/program/draft").status_code == 200


def test_all_21_tools_derive_action_schemas_from_real_routes(tmp_path):
    state = _mcp_state(object())
    app = create_app(state, security=_security(tmp_path))
    contracts = tool_contracts(app)
    assert len(contracts) == len(TOOL_ROUTES) == 21
    api = app.openapi()
    for tool in contracts:
        jsonschema.Draft202012Validator.check_schema(tool["inputSchema"])
        actions = TOOL_ROUTES[tool["name"]]
        assert len(tool["inputSchema"]["oneOf"]) == len(actions)
        for variant in tool["inputSchema"]["oneOf"]:
            action = variant["properties"]["action"]["const"]
            method, path = actions[action]
            assert method.lower() in api["paths"][path]
    # Runtime keys, controls and the advisory node preview are retained under
    # the intervention tool; the removed runtime-override tool is not served.
    assert "manyforge_runtime_override" not in TOOL_ROUTES
    intervention = next(t for t in contracts if t["name"] == "manyforge_intervention")
    assert {
        variant["properties"]["action"]["const"]
        for variant in intervention["inputSchema"]["oneOf"]
    } == {"state", "recover", "manual_result", "runtime_controls", "blackboard", "node_preview"}
    validator = next(t for t in contracts if t["name"] == "manyforge_program_validate")
    assert validator["annotations"]["readOnlyHint"]
    assert next(t for t in contracts if t["name"] == "manyforge_skills")["annotations"][
        "readOnlyHint"
    ]
    assert not next(t for t in contracts if t["name"] == "manyforge_kernel")["annotations"][
        "readOnlyHint"
    ]
    # Forwarded Stop and command routes still document their body for agents.
    for name, action in (
        ("manyforge_kernel", "stop"),
        ("manyforge_kernel", "stop_all"),
        ("manyforge_cycle_control", "command"),
    ):
        tool = next(t for t in contracts if t["name"] == name)
        variant = next(
            v for v in tool["inputSchema"]["oneOf"]
            if v["properties"]["action"]["const"] == action
        )
        assert "body" in variant["required"]
        assert "$defs" not in variant["properties"]["body"]


def test_command_tools_say_where_the_outcome_is_read(tmp_path):
    """OD-214: a command returns at acceptance; each command tool names the
    read action whose status carries the command's row."""

    contracts = {
        tool["name"]: tool
        for tool in tool_contracts(create_app(_mcp_state(object()), security=_security(tmp_path)))
    }
    reads = {
        "manyforge_cycle_control": ("status", "/api/cycle/status", "runner_commands"),
        "manyforge_intervention": ("state", "/api/cycle/status", "runner_commands"),
        "manyforge_deployment": ("workspace", "/api/deployment/workspace", "apply_commands"),
        "manyforge_scenario": ("status", "/api/deployment/workspace", "apply_commands"),
        "manyforge_program_load": ("apply_status", "/api/deployment/workspace", "apply_commands"),
        "manyforge_session": ("apply_status", "/api/deployment/workspace", "apply_commands"),
    }
    for name, (action, path, member) in reads.items():
        assert TOOL_ROUTES[name][action] == ("GET", path)
        description = contracts[name]["description"]
        # "returns" for one command, "return" for several (`load` and `new`).
        assert re.search(r"\breturns? at acceptance\b", description)
        assert f"`{member}` of `{action}`" in description
    # Tools without such commands keep the plain description.
    assert "acceptance" not in contracts["manyforge_kernel"]["description"]


def test_the_stop_tool_offers_stop_and_stop_all():
    kernel = TOOL_ROUTES["manyforge_kernel"]
    assert kernel["stop_all"] == ("POST", "/api/execution/stop-all")
    assert not {"full_stop", "halt"} & set(kernel)
    assert all(
        path not in {"/api/execution/halt", "/api/execution/full-stop"}
        for actions in TOOL_ROUTES.values()
        for _method, path in actions.values()
    )


def _command(kind: str) -> dict:
    return {"command_envelope": {"command_kind": kind}, "payload": {"marker": kind}}


@pytest.mark.parametrize(
    "kind",
    [
        "program_reset", "program_run", "program_motion_resume", "stop_all", "operation_stop",
        "halt", "program_resume_halt", "program_discard_halt",
    ],
)
def test_the_command_tool_forwards_every_kind_unchanged(tmp_path, kind):
    app = FastAPI()
    received = []

    @app.post("/api/control/commands")
    def commands(body: dict):
        received.append(body)
        return {"accepted": True}

    adapter = ComposerMcpAdapter(app, _security(tmp_path))
    response = asyncio.run(
        adapter.invoke(
            "manyforge_cycle_control", {"action": "command", "body": _command(kind)}
        )
    )
    assert response.json() == {"accepted": True}
    assert received == [_command(kind)]


def test_stop_kinds_and_the_stop_tool_still_reach_the_stop_path(tmp_path):
    owner = _StopOwner()
    state = _mcp_state(owner)
    stop = {"operations": [{"operation_id": "A"}]}
    with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
        for kind in ("stop_all", "operation_stop", "halt"):
            result = _call(
                client, "manyforge_cycle_control", {"action": "command", "body": _command(kind)}
            ).json()["result"]
            assert not result["isError"]
        for action, body in (("stop_all", {}), ("stop", stop)):
            result = _call(client, "manyforge_kernel", {"action": action, "body": body}).json()[
                "result"
            ]
            assert not result["isError"]
    assert owner.stops == [
        ("/api/control/commands", _command("stop_all")),
        ("/api/control/commands", _command("operation_stop")),
        ("/api/control/commands", _command("halt")),
        ("/api/execution/stop-all", {}),
        ("/api/execution/stops", stop),
    ]


def test_mcp_has_parity_with_the_component_model_and_apply_routes(tmp_path):
    """MCP reaches the same server-side operations as REST."""

    expected = {
        ("manyforge_scenario", "load"): ("POST", "/api/scenario/load"),
        ("manyforge_scenario", "new"): ("POST", "/api/scenario/new"),
        ("manyforge_deployment", "preview"): ("POST", "/api/deployment/preview"),
        ("manyforge_deployment", "apply"): ("POST", "/api/deployment/apply"),
        ("manyforge_deployment", "effective"): ("GET", "/api/deployment/effective"),
        ("manyforge_deployment", "revert_preview"): (
            "POST", "/api/deployment/revert/preview",
        ),
        ("manyforge_deployment", "revert"): ("POST", "/api/deployment/revert"),
        ("manyforge_components", "list"): ("GET", "/api/components"),
        ("manyforge_components", "action"): (
            "POST", "/api/components/{component_id}/actions",
        ),
        ("manyforge_components", "group_action"): ("POST", "/api/components/actions"),
        ("manyforge_components", "cell_latch"): ("GET", "/api/cell/latch"),
        ("manyforge_components", "readiness"): ("GET", "/api/program/readiness"),
        ("manyforge_io", "list"): ("GET", "/api/io"),
        ("manyforge_io", "set"): ("POST", "/api/io/{signal_id}/set"),
    }
    for (tool, action), route in expected.items():
        assert TOOL_ROUTES[tool][action] == route
    api = create_app(_mcp_state(object()), security=_security(tmp_path)).openapi()
    for method, path in expected.values():
        assert method.lower() in api["paths"][path], path
    routed = {route for actions in TOOL_ROUTES.values() for route in actions.values()}
    assert not any("/api/cclc/" in path for _method, path in routed)


def test_real_mcp_skill_expansion_uses_existing_headless_http_client(tmp_path):
    calls = []
    body = {
        "schema_version": "manyforge.skill_expansion_request.v2",
        "expected_environment_digest": "sha256:" + "1" * 64,
        "skill_id": "move_to_named_config",
        "instance_prefix": "selected",
        "arguments": {
            "configuration_name": "home", "goal": [{"joint_name": "joint1", "position_rad": 0.0}],
            "max_tcp_linear_speed_mps": 0.1,
            "arm_target": {"component_id": "robot", "command_target_id": "arm"},
            "base_frame": {"component_id": "robot", "frame_id": "base"},
        },
        "program_context": {},
    }
    owner_result = {
        "schema_version": "manyforge.skill_expansion.v2",
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


def test_mcp_kernel_diagnostics_pass_the_v4_check_counts_unchanged(tmp_path):
    # OD-217: the Composer proxy and the MCP tool forward the host's v4
    # status reply, per-robot collision-check counts included, as it came.
    reply = {
        "api_version": "manyforge.execution_command_response.v6",
        "command": "status",
        "ok": True,
        "status": {
            "control_state": {},
            "identity": {},
            "runtime_collision_checks": [{
                "check_period_ns": 10_000_000,
                "checks_late": 3,
                "checks_run": 4,
                "controllable_component_id": "lite6_robot",
                "periods_skipped": 4,
            }],
            "schema_version": "manyforge.execution_status.v4",
            "targets": {},
        },
    }
    calls = []

    def owner(request):
        calls.append((request.method, request.url.path))
        return httpx.Response(200, json=reply)

    with httpx.Client(
        base_url="http://headless.test", transport=httpx.MockTransport(owner)
    ) as transport:
        headless = HeadlessHttpClient("http://headless.test", client=transport)
        state = _mcp_state(headless)
        with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
            rest = client.get("/api/execution/diagnostics")
            assert rest.status_code == 200
            assert rest.json() == reply
            result = _call(client, "manyforge_kernel", {"action": "diagnostics"}).json()["result"]
            assert not result["isError"], result
            assert json.loads(result["content"][0]["text"]) == reply
    assert calls == [("GET", "/api/execution/diagnostics")] * 2


def test_real_mcp_initialize_discover_read_and_pure_validation(tmp_path):
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
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
        # Validation includes the Headless reference check against the
        # Deployment Draft; without one it is a typed refusal, never a change.
        assert validated["isError"]
        assert "deployment_draft_unavailable" in validated["content"][0]["text"]
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
        create_app(ComposerState(application_start_id=0x3F9A0000000000A1), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        result = _call(client, "manyforge_catalog", arguments).json()["result"]
        assert result["isError"]


def test_mcp_io_set_accepts_member_signals_like_rest(tmp_path):
    # A member signal is `<member_id>/<signal>`; REST's
    # `{signal_id:path}` takes it, so MCP does too, segment by segment.
    calls = []

    class Headless:
        def set_io_output(self, signal_id, value):
            calls.append((signal_id, value))
            return {"outcome": "succeeded"}

    app = FastAPI()
    app.include_router(create_headless_router(Headless(), object()))  # type: ignore[arg-type]
    adapter = ComposerMcpAdapter(app, _security(tmp_path))

    def set_output(signal_id: str):
        return adapter.invoke(
            "manyforge_io",
            {"action": "set", "path": {"signal_id": signal_id}, "body": {"value": True}},
        )

    async def exercise():
        assert (await set_output("controller_io/CO1")).json() == {"outcome": "succeeded"}
        for refused in ("controller_io/../CO1", "controller_io/CO1?x=1", "controller_io//CO1", "/CO1"):
            with pytest.raises(ValueError, match="invalid path component"):
                await set_output(refused)
        # A plain path parameter still takes one component only.
        with pytest.raises(ValueError, match="invalid path component"):
            await adapter.invoke(
                "manyforge_components",
                {
                    "action": "action",
                    "path": {"component_id": "controller_io/CO1"},
                    "body": {"action": "enable"},
                },
            )

    with TestClient(app):
        asyncio.run(exercise())
    assert calls == [("controller_io/CO1", True)]


def test_mcp_keeps_host_origin_and_request_size_bounds(tmp_path):
    with TestClient(
        create_app(ComposerState(application_start_id=0x3F9A0000000000A1), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        assert client.post("/mcp/", headers={"Host": "foreign.invalid"}, json={}).status_code == 421
        assert (
            client.post("/mcp/", headers={"Origin": "http://foreign.invalid"}, json={}).status_code
            == 403
        )
        assert client.post("/mcp/", content=b"x" * (1024 * 1024 + 1)).status_code == 413


@pytest.mark.parametrize("body", [{}, [], "malformed", True, None])
def test_runtime_failure_and_malformed_stop_keep_real_route_semantics(tmp_path, body):
    owner = _StopOwner()
    state = _mcp_state(owner)
    with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
        invalid = _call(
            client, "manyforge_cycle_control", {"action": "command", "body": {}}
        ).json()["result"]
        assert invalid["isError"]
        stop = _call(client, "manyforge_kernel", {"action": "stop_all", "body": body}).json()[
            "result"
        ]
        assert not stop["isError"]
        assert json.loads(stop["content"][0]["text"])["accepted"]
        # A body that is not a JSON object reaches Headless as `{}`.
        assert owner.stops == [("/api/execution/stop-all", {})]


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

    @app.post("/api/execution/stop-all")
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
                adapter.invoke("manyforge_kernel", {"action": "stop_all", "body": {}}),
                1,
            )
            assert response.json()["accepted"]
        finally:
            release.set()
        assert (await pending).json()["accepted"]

    asyncio.run(exercise())
    assert calls == ["ordinary", "stop"]


def test_validation_endpoint_is_also_available_without_mcp(tmp_path):
    state = ComposerState(application_start_id=0x3F9A0000000000A1)
    with TestClient(create_app(state, security=_security(tmp_path))) as client:
        before = client.get("/api/program/draft").json()
        response = client.post(
            "/api/program/validate",
            json={"authoring": empty_authoring(state.node_catalog)},
        )
        # Reference validation needs a Deployment Draft; without one the
        # endpoint refuses with its typed code and changes nothing.
        assert response.status_code == 409
        assert response.json()["detail"]["code"] == "deployment_draft_unavailable"
        assert client.get("/api/program/draft").json() == before


def test_real_mcp_stop_bypasses_a_saturated_ordinary_worker_pool(tmp_path):
    app = create_app(
        _mcp_state(_StopOwner()),
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
                                    "arguments": {"action": "stop_all", "body": {}},
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
    owner = _StopOwner()
    state = _mcp_state(owner)
    with TestClient(create_app(state, security=_security(tmp_path), enable_mcp=True)) as client:
        response = client.post(
            "/mcp/",
            headers={
                "Accept": "application/json, text/event-stream",
                "Content-Type": "application/json",
            },
            content='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"manyforge_kernel","arguments":{"action":"stop_all","body":{"bad":NaN}}}}',
        )
        assert not response.json()["result"]["isError"]
        assert owner.stops == [("/api/execution/stop-all", {})]


@pytest.mark.parametrize("action", ["stop", "stop_all"])
@pytest.mark.parametrize(
    "body",
    [
        '{"operations":[{"operation_id":"A"}],"operations":[{"operation_id":"B"}]}',
        '{"operations":[{"operation_id":"A","operation_id":"B"}]}',
    ],
)
def test_raw_duplicate_stop_selectors_reach_malformed_containment(tmp_path, action, body):
    owner = _StopOwner()
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
        # The ambiguous selector is never narrowed: Headless gets `{}`.
        assert json.loads(result["content"][0]["text"])["forwarded"] == {}
        assert owner.stops == [
            ("/api/execution/" + ("stops" if action == "stop" else "stop-all"), {})
        ]


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
    context = {"request_key": "5" * 32, "source_label": "mcp"}
    target = lambda identity: {
        "operation_id": identity,
        "expected_operation_revision": 1,
    }
    raw_body = (
        '{"request_context":'
        + json.dumps(context)
        + ',"operations":'
        + json.dumps([target(1)])
        + ',"operations":'
        + json.dumps([target(2)])
        + "}"
    )
    # Pin the counterfactual: last-member-wins decoding would keep only 2.
    assert json.loads(raw_body)["operations"][0]["operation_id"] == 2
    owner = _StopOwner()
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
        # Forwarded as `{}` (Headless promotes it), never as the scoped Stop.
        assert owner.stops == [("/api/execution/stops", {})]


# Each is turned into `null` by the MCP SDK (OD-232).
_NONFINITE_NUMBERS = ["NaN", "Infinity", "-Infinity", "1e999", "-1e999"]


class _Recorder:
    """Headless double: records every request it is asked to forward."""

    def __init__(self):
        self.requests = []

    def request(self, method, path, *, params=(), body=None, timeout_seconds=None):
        self.requests.append((method, path, body))
        return {"accepted": True, "forwarded": body}


def _record_invoke(monkeypatch):
    """Replace the adapter's forward with a recorder of what it would carry out."""

    invoked = []

    async def invoke(self, name, arguments):
        invoked.append((name, arguments))
        return httpx.Response(200, json={"ok": True})

    monkeypatch.setattr(ComposerMcpAdapter, "invoke", invoke)
    return invoked


def _raw_call(client: TestClient, name: str, arguments: str):
    return client.post(
        "/mcp/",
        headers={
            "Accept": "application/json, text/event-stream",
            "Content-Type": "application/json",
        },
        content='{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":'
        + json.dumps(name)
        + ',"arguments":'
        + arguments
        + "}}",
    )


@pytest.mark.parametrize("constant", _NONFINITE_NUMBERS)
@pytest.mark.parametrize(
    "name, arguments",
    [
        # Read tools: a top-level and a nested query member.
        ("manyforge_kernel", '{"action":"targets","query":{"limit":%s}}'),
        ("manyforge_kernel", '{"action":"targets","query":{"filter":{"a":[1,%s]}}}'),
        # Command tools: the body itself, a top-level and a nested member.
        ("manyforge_kernel", '{"action":"submit","body":%s}'),
        ("manyforge_kernel", '{"action":"submit","body":{"speed":%s}}'),
        ("manyforge_kernel", '{"action":"submit","body":{"motion":{"targets":[{"x":%s}]}}}'),
        ("manyforge_cycle_control", '{"action":"command","body":{"speed_override":%s}}'),
        (
            "manyforge_cycle_control",
            '{"action":"command","body":{"command_envelope":'
            '{"command_kind":"execute","arguments":{"value":%s}}}}',
        ),
        # Authoring tools: a top-level and a nested body member.
        ("manyforge_program_tree", '{"action":"mutate","body":{"revision":%s}}'),
        (
            "manyforge_program_tree",
            '{"action":"mutate","body":{"mutations":[{"params":{"speed":%s}}]}}',
        ),
    ],
)
def test_nonfinite_numbers_refuse_every_tool_call_but_a_stop(
    tmp_path, monkeypatch, constant, name, arguments
):
    # OD-232: the SDK would turn the value into `null`; the call is refused
    # with the duplicate-member answer and nothing is forwarded or carried out.
    invoked = _record_invoke(monkeypatch)
    owner = _Recorder()
    with TestClient(
        create_app(_mcp_state(owner), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        response = _raw_call(client, name, arguments % constant)
    assert response.status_code == 400
    assert response.json() == {
        "jsonrpc": "2.0",
        "id": None,
        "error": {"code": -32600, "message": "Invalid or ambiguous MCP JSON request"},
    }
    assert invoked == []
    assert owner.requests == []


@pytest.mark.parametrize("constant", _NONFINITE_NUMBERS)
@pytest.mark.parametrize("action", ["stop", "stop_all"])
@pytest.mark.parametrize("body", ['{"bad":%s}', '{"operations":[{"operation_id":%s}]}'])
def test_nonfinite_native_stop_still_reaches_the_owner_widened(
    tmp_path, constant, action, body
):
    owner = _StopOwner()
    with TestClient(
        create_app(_mcp_state(owner), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        response = _raw_call(
            client,
            "manyforge_kernel",
            '{"action":' + json.dumps(action) + ',"body":' + body % constant + "}",
        )
    assert not response.json()["result"]["isError"]
    assert owner.stops == [
        ("/api/execution/" + ("stops" if action == "stop" else "stop-all"), {})
    ]


@pytest.mark.parametrize("constant", _NONFINITE_NUMBERS)
@pytest.mark.parametrize(
    "kind", ["program_motion_stop", "operation_stop", "stop_all", "halt"]
)
@pytest.mark.parametrize(
    "payload",
    ['{"request_key":%s}', '{"operations":[{"operation_id":"A","revision":%s}]}'],
)
def test_nonfinite_control_stop_command_is_sent_on_as_the_widest_stop(
    tmp_path, monkeypatch, constant, kind, payload
):
    # A Stop is never refused, and an unreadable Stop becomes the widest one:
    # the payload is sent on empty, which Headless turns into Stop all (a Halt
    # stays a Halt). The sender's envelope is kept.
    invoked = _record_invoke(monkeypatch)
    envelope = {"command_kind": kind, "source_label": "mcp", "revision": 2.5}
    with TestClient(
        create_app(_mcp_state(_Recorder()), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        response = _raw_call(
            client,
            "manyforge_cycle_control",
            '{"action":"command","body":{"command_envelope":'
            + json.dumps(envelope)
            + ',"payload":'
            + payload % constant
            + "}}",
        )
    assert not response.json()["result"]["isError"]
    assert invoked == [
        (
            "manyforge_cycle_control",
            {"action": "command", "body": {"command_envelope": envelope, "payload": {}}},
        )
    ]


@pytest.mark.parametrize("constant", _NONFINITE_NUMBERS)
def test_nonfinite_in_a_control_stop_envelope_passes_as_it_came(
    tmp_path, monkeypatch, constant
):
    # Not widened by the adapter: an envelope cannot be completed for the
    # sender. Pinned as it is; the SDK has turned the value into `null`.
    invoked = _record_invoke(monkeypatch)
    with TestClient(
        create_app(_mcp_state(_Recorder()), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        response = _raw_call(
            client,
            "manyforge_cycle_control",
            '{"action":"command","body":{"command_envelope":'
            '{"command_kind":"stop_all","extra":' + constant + '},"payload":{}}}',
        )
    assert not response.json()["result"]["isError"]
    assert invoked == [
        (
            "manyforge_cycle_control",
            {
                "action": "command",
                "body": {
                    "command_envelope": {"command_kind": "stop_all", "extra": None},
                    "payload": {},
                },
            },
        )
    ]


def test_finite_numbers_and_real_nulls_still_pass(tmp_path, monkeypatch):
    invoked = _record_invoke(monkeypatch)
    arguments = {
        "action": "mutate",
        "body": {"revision": 3, "speed": -1.5e3, "note": None, "list": [0.0, None, 1e308]},
    }
    with TestClient(
        create_app(_mcp_state(_Recorder()), security=_security(tmp_path), enable_mcp=True)
    ) as client:
        response = _call(client, "manyforge_program_tree", arguments)
    assert not response.json()["result"]["isError"]
    assert invoked == [("manyforge_program_tree", arguments)]
