"""Optional stateless MCP transport over the existing Composer HTTP owners.

The closed table is routing, not execution policy. Requests traverse the real
ASGI routes, including their DTOs, attribution, freshness and reserved Stop
capacity. No MCP session, command receipt, retry loop or worker is retained.
"""

from __future__ import annotations

import base64
from copy import deepcopy
import json
import re
from typing import Any

import httpx
from fastapi import FastAPI
from pydantic import BaseModel, ConfigDict, Field
from starlette.concurrency import run_in_threadpool
from starlette.convertors import PathConvertor
from starlette.requests import Request
from starlette.responses import JSONResponse
from starlette.routing import Route
from starlette.types import Receive, Scope, Send
from manyforge_headless_api.control_protocol import is_stop_command

from .security import SecurityConfig

# One routing inventory, also used to generate the public tool contract.
# Each action names an existing route; callers never supply a URL or method.
TOOL_ROUTES: dict[str, dict[str, tuple[str, str]]] = {
    "manyforge_scene_state": {"read": ("GET", "/api/scene")},
    "manyforge_scene_edit": {"transact": ("POST", "/api/scene/transactions")},
    "manyforge_program_load": {
        "open_draft": ("POST", "/api/program/open-draft"),
        "new_draft": ("POST", "/api/program/new-draft"),
        "apply": ("POST", "/api/program/apply"),
    },
    "manyforge_program_tree": {
        "read": ("GET", "/api/program/draft"),
        "mutate": ("POST", "/api/program/draft/mutations"),
    },
    "manyforge_program_validate": {
        "current": ("GET", "/api/program/draft"),
        "source": ("POST", "/api/program/validate"),
    },
    "manyforge_cycle_control": {
        "state": ("GET", "/api/control/state"),
        "command": ("POST", "/api/control/commands"),
    },
    "manyforge_capabilities": {
        "environment": ("GET", "/api/node-environment"),
        "targets": ("GET", "/api/execution/targets"),
    },
    "manyforge_skills": {
        "catalog": ("GET", "/api/skills"),
        "expand": ("POST", "/api/skills/expand"),
        "assistant_modes": ("GET", "/api/assistant/modes"),
    },
    "manyforge_intervention": {
        "state": ("GET", "/api/cycle/status"),
        "recover": ("POST", "/api/cycle/recovery-actions"),
        "manual_result": ("POST", "/api/program/runs/{program_run_id}/manual-result"),
        "runtime_controls": (
            "GET",
            "/api/program/runs/{program_run_id}/runtime-controls",
        ),
        "blackboard": ("POST", "/api/program/runs/{program_run_id}/blackboard-values"),
        "node_preview": (
            "POST",
            "/api/program/runs/{program_run_id}/node-correction-previews",
        ),
    },
    "manyforge_execution_status": {
        "state": ("GET", "/api/control/state"),
        "run": ("GET", "/api/program/runs/{program_run_id}"),
        "operation": ("GET", "/api/execution/operations/{operation_id}"),
    },
    "manyforge_debug_export": {
        "export": ("GET", "/api/debug/export"),
        "trace": ("GET", "/api/debug/export/ros2-medkit"),
        "history": ("GET", "/api/execution/history"),
        "event": ("GET", "/api/execution/history/{sequence}"),
        "health": ("GET", "/api/execution/history/health"),
        "program_recorder": ("GET", "/api/program/diagnostics/health"),
    },
    "manyforge_catalog": {
        "list": ("GET", "/api/catalog"),
        "entry": ("GET", "/api/catalog/{entry_id}"),
    },
    "manyforge_scene_snapshot": {
        "capture": ("GET", "/api/scene/snapshot/render"),
        "status": ("GET", "/api/scene/snapshot/status"),
    },
    "manyforge_program_save": {
        "save": ("POST", "/api/program/save"),
        "save_as": ("POST", "/api/program/save-as"),
    },
    "manyforge_files": {"browse": ("GET", "/api/files/browse")},
    "manyforge_deployment": {
        "workspace": ("GET", "/api/deployment/workspace"),
        "component_catalog": ("GET", "/api/deployment/component-catalog"),
        "open_draft": ("POST", "/api/deployment/open-draft"),
        "mutate": ("POST", "/api/deployment/draft/mutations"),
        "add_components": ("POST", "/api/deployment/draft/components"),
        "stop_policy": ("POST", "/api/deployment/draft/controlled-stop"),
        "motion_limit": ("POST", "/api/deployment/draft/motion-target-limit"),
        "preview": ("POST", "/api/deployment/preview"),
        "apply": ("POST", "/api/deployment/apply"),
        "effective": ("GET", "/api/deployment/effective"),
        "save": ("POST", "/api/deployment/save"),
        "save_as": ("POST", "/api/deployment/save-as"),
        "discard": ("POST", "/api/deployment/discard"),
        "revert_preview": ("POST", "/api/deployment/revert/preview"),
        "revert": ("POST", "/api/deployment/revert"),
        "manager": ("GET", "/api/manager/status"),
        "graph": ("GET", "/api/manager/graph"),
    },
    # The same server-side Load and New as the GUI.
    "manyforge_scenario": {
        "load": ("POST", "/api/scenario/load"),
        "new": ("POST", "/api/scenario/new"),
    },
    "manyforge_components": {
        "list": ("GET", "/api/components"),
        "action": ("POST", "/api/components/{component_id}/actions"),
        "group_action": ("POST", "/api/components/actions"),
        "cell_latch": ("GET", "/api/cell/latch"),
        "readiness": ("GET", "/api/program/readiness"),
    },
    "manyforge_io": {
        "list": ("GET", "/api/io"),
        "set": ("POST", "/api/io/{signal_id}/set"),
    },
    "manyforge_kernel": {
        "identity": ("GET", "/api/execution/identity"),
        "targets": ("GET", "/api/execution/targets"),
        "state": ("GET", "/api/execution/control-state"),
        "diagnostics": ("GET", "/api/execution/diagnostics"),
        "motion_preview": ("POST", "/api/execution/motion-previews"),
        "capability_preview": ("POST", "/api/execution/capability-command-previews"),
        "submit": ("POST", "/api/execution/operations"),
        "operation": ("GET", "/api/execution/operations/{operation_id}"),
        "reset": ("POST", "/api/execution/operations/{operation_id}/reset"),
        "stop": ("POST", "/api/execution/stops"),
        "full_stop": ("POST", "/api/execution/full-stop"),
        "scale": ("POST", "/api/execution/runtime-scale-control"),
        "read_scale": ("GET", "/api/execution/runtime-scale-control"),
    },
    "manyforge_session": {
        "status": ("GET", "/api/program/workspace"),
        "discard": ("POST", "/api/program/discard"),
        "revert": ("POST", "/api/program/revert"),
    },
}


class ToolArguments(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)

    action: str = Field(min_length=1, max_length=64)
    path: dict[str, str] = Field(default_factory=dict, max_length=4)
    query: dict[str, Any] = Field(default_factory=dict, max_length=32)
    # The route, not this transport envelope, owns payload validation. In
    # particular a recognized Stop must reach its malformed-body handler.
    body: Any = None


def _reserved_stop(path: str, body: Any) -> bool:
    if path in {"/api/execution/stops", "/api/execution/full-stop"}:
        return True
    if path == "/api/control/commands" and isinstance(body, dict):
        envelope = body.get("command_envelope")
        return (
            isinstance(envelope, dict)
            and isinstance(envelope.get("command_kind"), str)
            and is_stop_command(envelope["command_kind"])
        )
    return False


class _JsonObject(dict):
    def __init__(self, pairs):
        super().__init__(pairs)
        self.duplicate = len(pairs) != len(self)


def _has_duplicate(value: Any) -> bool:
    if isinstance(value, _JsonObject):
        return value.duplicate or any(_has_duplicate(v) for v in value.values())
    return isinstance(value, list) and any(_has_duplicate(v) for v in value)


def _checked_envelope(body: bytes) -> bytes:
    # The SDK uses last-member-wins JSON decoding. Never let that silently
    # narrow a Stop selector or change another effect. Only an unambiguous
    # native Stop route may forward a malformed body to its existing handler.
    envelope = json.loads(body, object_pairs_hook=_JsonObject)
    if not _has_duplicate(envelope):
        return body
    if isinstance(envelope, dict) and envelope.get("method") == "tools/call":
        params = envelope.get("params")
        arguments = params.get("arguments") if isinstance(params, dict) else None
        if isinstance(arguments, dict):
            arguments.pop("body", None)
            name, action = params.get("name"), arguments.get("action")
            route = (
                TOOL_ROUTES.get(name, {}).get(action)
                if isinstance(name, str) and isinstance(action, str)
                else None
            )
            if route in {
                ("POST", "/api/execution/stops"),
                ("POST", "/api/execution/full-stop"),
            } and not _has_duplicate(envelope):
                arguments["body"] = None
                return json.dumps(envelope, allow_nan=False).encode("utf-8")
    raise ValueError("duplicate JSON members in MCP request")


def tool_contracts(app: FastAPI) -> list[dict[str, Any]]:
    """Derive action schemas from actual routes, never a second DTO copy."""
    api = app.openapi()
    result = []
    for name, actions in TOOL_ROUTES.items():
        variants = []
        definitions: dict[str, Any] = {}

        def local_refs(value: Any) -> Any:
            if isinstance(value, list):
                return [local_refs(item) for item in value]
            if not isinstance(value, dict):
                return value
            value = deepcopy(value)
            ref = value.get("$ref", "")
            if ref.startswith("#/components/schemas/"):
                key = ref.rsplit("/", 1)[1]
                value["$ref"] = f"#/$defs/{key}"
                if key not in definitions:
                    definitions[key] = {}  # allow recursive DTO definitions
                    definitions[key] = local_refs(api["components"]["schemas"][key])
            return {key: local_refs(item) for key, item in value.items()}

        read_only = True
        for action, (method, path) in actions.items():
            operation = api["paths"].get(path, {}).get(method.lower())
            if operation is None:
                continue  # unconfigured runtime routes are not advertised
            properties: dict[str, Any] = {"action": {"const": action, "type": "string"}}
            required = ["action"]
            for location in ("path", "query"):
                parameters = [p for p in operation.get("parameters", []) if p["in"] == location]
                if parameters:
                    keys = [p["name"] for p in parameters if p.get("required")]
                    properties[location] = {
                        "type": "object",
                        "additionalProperties": False,
                        "properties": {p["name"]: local_refs(p["schema"]) for p in parameters},
                        "required": keys,
                    }
                    # URL path components are string-encoded, including counters.
                    if location == "path":
                        properties[location]["properties"] = {
                            p["name"]: {
                                "type": "string",
                                "minLength": 1,
                                "maxLength": 512,
                            }
                            for p in parameters
                        }
                    if keys:
                        required.append(location)
            request = operation.get("requestBody", {})
            if "application/json" in request.get("content", {}):
                properties["body"] = local_refs(request["content"]["application/json"]["schema"])
                if request.get("required"):
                    required.append("body")
            read_only &= method == "GET" or path in {
                "/api/program/validate",
                "/api/skills/expand",
            }
            variants.append(
                {
                    "type": "object",
                    "additionalProperties": False,
                    "description": f"{method} {path}",
                    "properties": properties,
                    "required": required,
                }
            )
        if variants:
            result.append(
                {
                    "name": name,
                    "description": (
                        "Thin ManyForge API adapter. Submit once; observe owner state. "
                        "Bodies use the unchanged REST DTOs. No implicit Apply, retry, "
                        "confirmation or identity selection. See each action's schema."
                    ),
                    "inputSchema": {
                        "type": "object",
                        "oneOf": variants,
                        "$defs": definitions,
                    },
                    "annotations": {
                        "readOnlyHint": read_only,
                        "destructiveHint": not read_only,
                        "idempotentHint": read_only,
                        "openWorldHint": False,
                    },
                }
            )
    return result


class ComposerMcpAdapter:
    def __init__(self, app: FastAPI, security: SecurityConfig) -> None:
        # Optional dependency: normal Composer/headless startup never imports it.
        from mcp import types
        from mcp.server.lowlevel import Server
        from mcp.server.transport_security import TransportSecuritySettings

        self._app = app
        # The real routes' `{name:path}` parameters (e.g. a member signal
        # `<member_id>/<signal>` in `/api/io/{signal_id:path}/set`): their
        # values hold `/`, and each segment is checked as a path component.
        self._multi_segment = {
            route.path_format: {
                key
                for key, convertor in route.param_convertors.items()
                if isinstance(convertor, PathConvertor)
            }
            for route in app.routes
            if isinstance(route, Route)
        }
        server = Server("ManyForge", version="0.2.1")
        contracts = [types.Tool.model_validate(value) for value in tool_contracts(app)]

        @server.list_tools()
        async def list_tools():
            return contracts

        # The actual HTTP routes validate the payload, including malformed
        # recognized Stop. Do not preempt that rule with SDK JSON-schema checks.
        @server.call_tool(validate_input=False)
        async def call_tool(name: str, arguments: dict[str, Any]):
            response = await self.invoke(name, arguments)
            if response.is_success and response.headers.get("content-type", "").startswith(
                "image/"
            ):
                return [
                    types.ImageContent(
                        type="image",
                        mimeType=response.headers["content-type"],
                        data=base64.b64encode(response.content).decode("ascii"),
                    )
                ]
            route = TOOL_ROUTES[name][arguments["action"]][1]
            text = (
                response.content.decode("utf-8")
                if _reserved_stop(route, arguments.get("body"))
                else await run_in_threadpool(response.content.decode, "utf-8")
            )
            return types.CallToolResult(
                content=[types.TextContent(type="text", text=text)],
                isError=not response.is_success,
            )

        self._server = server
        self._security = TransportSecuritySettings(
            allowed_hosts=security.mcp_allowed_hosts,
            allowed_origins=security.cors_origins,
        )

    async def handle_request(self, scope: Scope, receive: Receive, send: Send) -> None:
        from mcp.server.streamable_http_manager import StreamableHTTPSessionManager

        if scope["type"] == "http" and scope["method"] == "POST":
            # The outer Composer middleware already bounds this to 1 MiB.
            # Keep the check off the ordinary worker pool so Stop cannot queue.
            try:
                body = _checked_envelope(await Request(scope, receive).body())
            except (ValueError, UnicodeError, RecursionError):
                await JSONResponse(
                    {
                        "jsonrpc": "2.0",
                        "id": None,
                        "error": {
                            "code": -32600,
                            "message": "Invalid or ambiguous MCP JSON request",
                        },
                    },
                    status_code=400,
                )(scope, receive, send)
                return
            upstream_receive = receive
            delivered = False

            async def replay_receive():
                nonlocal delivered
                if delivered:
                    return await upstream_receive()
                delivered = True
                return {"type": "http.request", "body": body, "more_body": False}

            receive = replay_receive

        # SDK 1.27's stateless handler terminates transport only on normal
        # return. A request-local task group also closes it on cancellation or
        # send failure; nothing is retained until application shutdown.
        manager = StreamableHTTPSessionManager(
            self._server,
            stateless=True,
            json_response=True,
            security_settings=self._security,
        )
        async with manager.run():
            await manager.handle_request(scope, receive, send)

    async def invoke(self, name: str, arguments: dict[str, Any]) -> httpx.Response:
        args = ToolArguments.model_validate(arguments)
        route = TOOL_ROUTES.get(name, {}).get(args.action)
        if route is None:
            raise ValueError("unknown ManyForge tool or action")
        method, path = route
        expected = set(re.findall(r"\{([^{}]+)\}", path))
        if set(args.path) != expected:
            raise ValueError("path fields do not match this action")
        multi_segment = self._multi_segment.get(path, set())
        for key, value in args.path.items():
            segments = value.split("/") if key in multi_segment else [value]
            if len(value) > 512 or any(
                segment in {".", ".."}
                or re.fullmatch(r"[A-Za-z0-9_.:-]+", segment) is None
                for segment in segments
            ):
                raise ValueError("invalid path component")
            path = path.replace("{" + key + "}", value)
        if method == "GET" and args.body is not None:
            raise ValueError("read actions do not accept a body")
        # No network round trip or alternate route policy. The existing app
        # preserves sync-route offloading and execution/Stop lane selection.
        async with httpx.AsyncClient(
            transport=httpx.ASGITransport(app=self._app),
            base_url="http://manyforge.internal",
            follow_redirects=False,
        ) as client:

            def encode_body() -> bytes:
                return json.dumps(args.body, allow_nan=False, separators=(",", ":")).encode("utf-8")

            body = None
            if args.body is not None:
                if _reserved_stop(path, args.body):
                    # Stop cannot wait for the ordinary AnyIO pool. Both MCP
                    # and REST already cap this envelope at 1 MiB.
                    try:
                        body = encode_body()
                    except (ValueError, TypeError):
                        body = b"null"  # real route handles recognized malformed Stop
                else:
                    body = await run_in_threadpool(encode_body)
            return await client.request(
                method,
                path,
                params=args.query,
                content=body,
                headers={"Content-Type": "application/json"},
            )
