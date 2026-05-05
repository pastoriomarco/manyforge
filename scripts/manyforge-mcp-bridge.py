#!/usr/bin/env python3
"""ManyForge mode-scoped MCP wrapper for OpenShell sandboxes.

Translates MCP JSON-RPC over stdio (what OpenClaw consumes) into HTTP calls
to ManyForge's hardened assistant-bridge endpoint, exposing only the tools
the active assistant mode permits.

Why this exists:
    The broad ``/api/mcp`` surface in ManyForge is operator-grade and does
    not carry the bounded-autonomy contract (assistantMode + catalogHash +
    node allowlist + scope rules). Routing the in-sandbox composer
    assistant directly through that surface would bypass the runtime
    hardening on ``/api/assistant/bridge/tools/{toolId}``.

    This wrapper enforces the mode contract from the sandbox side:
      * ``tools/list`` returns only the tools listed in the mode's
        ``assistant_modes[<mode>].tools`` allowlist (fetched from
        Composer's ``/api/assistant/modes/{mode}``).
      * ``tools/call`` posts to the bridge endpoint with a full envelope
        (assistantMode, catalogHash, requestId, conversationId, principal),
        so server-side enforcement remains the source of truth.
      * Bookkeeping: a stable ``conversationId`` per stdio session and a
        fresh ``requestId`` per tool call give the audit trail something
        to attribute the call to.

Configuration (env vars):
    MANYFORGE_COMPOSER_BASE   — Composer HTTP root.
                                default: http://host.openshell.internal:9000
    MANYFORGE_ASSISTANT_MODE  — assistant mode to expose.
                                default: composer-assistant
    MANYFORGE_PRINCIPAL       — principal name recorded in audit.
                                default: openclaw-sandbox
    MANYFORGE_REQUEST_TIMEOUT — per-HTTP-call timeout in seconds.
                                default: 120
    MANYFORGE_CA_BUNDLE       — TLS CA bundle path.
                                default: /etc/openshell-tls/ca-bundle.pem if present

Usage in openclaw.json mcpServers:
    {"mcpServers": {"manyforge": {"command": "python3",
        "args": ["/sandbox/.manyforge/manyforge-mcp-bridge.py"]}}}
"""
from __future__ import annotations

import json
import os
import ssl
import sys
import time
import urllib.error
import urllib.request
import uuid
from typing import Any


COMPOSER_BASE = os.environ.get(
    "MANYFORGE_COMPOSER_BASE",
    "http://host.openshell.internal:9000",
).rstrip("/")
ASSISTANT_MODE = os.environ.get("MANYFORGE_ASSISTANT_MODE", "composer-assistant")
PRINCIPAL = os.environ.get("MANYFORGE_PRINCIPAL", "openclaw-sandbox")
REQUEST_TIMEOUT = float(os.environ.get("MANYFORGE_REQUEST_TIMEOUT", "120"))

_CA_BUNDLE = os.environ.get("MANYFORGE_CA_BUNDLE", "/etc/openshell-tls/ca-bundle.pem")
_SSL_CTX: ssl.SSLContext | None = None

# Stable per-process identity. OpenClaw spawns one bridge per agent
# session, so this maps cleanly to "one conversation".
CONVERSATION_ID = f"openclaw-{uuid.uuid4().hex[:12]}"

# Cached mode manifest — fetched lazily on first tools/list, refreshed on
# 409 catalog-hash mismatches (deployment hot-reload window).
_MODE_MANIFEST: dict[str, Any] | None = None


def _ssl_context() -> ssl.SSLContext:
    global _SSL_CTX
    if _SSL_CTX is None:
        _SSL_CTX = ssl.create_default_context()
        if os.path.exists(_CA_BUNDLE):
            _SSL_CTX.load_verify_locations(_CA_BUNDLE)
    return _SSL_CTX


def _http(method: str, path: str, body: dict[str, Any] | None = None) -> dict[str, Any]:
    """One HTTP round-trip. Returns parsed JSON, or raises with the body text."""
    url = f"{COMPOSER_BASE}{path}"
    data = json.dumps(body).encode("utf-8") if body is not None else None
    headers = {"Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    ctx = _ssl_context() if url.startswith("https://") else None
    try:
        resp = urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT, context=ctx)
        return json.loads(resp.read())
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        try:
            payload = json.loads(raw)
        except (ValueError, TypeError):
            payload = {"detail": raw[:1024]}
        # Re-raise as a typed exception so call sites can branch on status.
        raise _HttpError(exc.code, payload) from exc


class _HttpError(Exception):
    def __init__(self, status: int, payload: dict[str, Any]) -> None:
        super().__init__(f"HTTP {status}: {payload}")
        self.status = status
        self.payload = payload


def _refresh_mode_manifest() -> dict[str, Any]:
    """Fetch the mode-scoped tool/node manifest from Composer."""
    global _MODE_MANIFEST
    manifest = _http("GET", f"/api/assistant/modes/{ASSISTANT_MODE}")
    _MODE_MANIFEST = manifest
    return manifest


def _get_mode_manifest() -> dict[str, Any]:
    if _MODE_MANIFEST is None:
        return _refresh_mode_manifest()
    return _MODE_MANIFEST


def _mcp_tool_descriptor(tool: dict[str, Any]) -> dict[str, Any]:
    """Translate a Composer mode-tool entry into MCP tool/list shape."""
    description = tool.get("description") or ""
    effect = tool.get("effect")
    if isinstance(effect, str) and effect:
        # Append the effect tag so the model can route by inspection of the
        # tool list itself; mirrors the deployment YAML's scope-tag pattern.
        description = f"{description}\n\n[effect: {effect}]"
    return {
        "name": tool.get("id"),
        "description": description,
        "inputSchema": tool.get("inputSchema") or {
            "type": "object",
            "additionalProperties": True,
        },
    }


def _handle_initialize(message: dict[str, Any]) -> dict[str, Any]:
    return {
        "jsonrpc": "2.0",
        "id": message.get("id"),
        "result": {
            "protocolVersion": "2024-11-05",
            "capabilities": {"tools": {}},
            "serverInfo": {
                "name": "manyforge-composer",
                "version": "0.2.0",
                "mode": ASSISTANT_MODE,
            },
        },
    }


def _handle_tools_list(message: dict[str, Any]) -> dict[str, Any]:
    try:
        manifest = _get_mode_manifest()
    except _HttpError as exc:
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {
                "code": -32000,
                "message": (
                    f"Could not load assistant mode '{ASSISTANT_MODE}' from "
                    f"{COMPOSER_BASE}: HTTP {exc.status}"
                ),
                "data": exc.payload,
            },
        }
    tools = [_mcp_tool_descriptor(t) for t in manifest.get("tools") or []]
    return {
        "jsonrpc": "2.0",
        "id": message.get("id"),
        "result": {"tools": tools},
    }


def _handle_tools_call(message: dict[str, Any]) -> dict[str, Any]:
    params = message.get("params") or {}
    tool_id = params.get("name")
    arguments = params.get("arguments") or {}
    if not isinstance(tool_id, str) or not tool_id:
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {"code": -32602, "message": "tools/call requires a tool name"},
        }

    manifest = _get_mode_manifest()
    allowed_ids = {
        t.get("id") for t in (manifest.get("tools") or []) if isinstance(t.get("id"), str)
    }
    if tool_id not in allowed_ids:
        # Hard-stop client-side too. The bridge endpoint will reject it,
        # but failing here gives the model a clearer, smaller error.
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {
                "code": -32601,
                "message": (
                    f"tool '{tool_id}' is not exposed by assistant mode "
                    f"'{ASSISTANT_MODE}'"
                ),
                "data": {"availableTools": sorted(allowed_ids)},
            },
        }

    catalog_hash = manifest.get("catalogHash")
    request_id = f"openclaw-{int(time.time() * 1000)}-{uuid.uuid4().hex[:8]}"
    envelope = {
        "requestId": request_id,
        "conversationId": CONVERSATION_ID,
        "assistantMode": ASSISTANT_MODE,
        "catalogHash": catalog_hash,
        "principal": PRINCIPAL,
        "arguments": arguments,
    }

    try:
        body = _http("POST", f"/api/assistant/bridge/tools/{tool_id}", envelope)
    except _HttpError as exc:
        if exc.status == 409 and "catalogHash" in str(exc.payload):
            # Deployment was hot-reloaded; refresh and retry once.
            try:
                _refresh_mode_manifest()
                envelope["catalogHash"] = (_MODE_MANIFEST or {}).get("catalogHash")
                body = _http("POST", f"/api/assistant/bridge/tools/{tool_id}", envelope)
            except _HttpError as retry_exc:
                return _bridge_error_to_mcp(message, retry_exc)
        else:
            return _bridge_error_to_mcp(message, exc)

    # Translate ManyForge bridge response → MCP tool result.
    success = bool(body.get("success"))
    payload = body.get("result") if isinstance(body.get("result"), dict) else {}
    error_text = body.get("error") if isinstance(body.get("error"), str) else None

    content_text = json.dumps(
        {
            "success": success,
            "result": payload,
            "warnings": payload.get("warnings") or body.get("warnings") or [],
            "error": error_text,
            "draftMutated": body.get("draftMutated", False),
            "requestId": request_id,
        },
        sort_keys=False,
    )
    return {
        "jsonrpc": "2.0",
        "id": message.get("id"),
        "result": {
            "content": [{"type": "text", "text": content_text}],
            "isError": (not success),
        },
    }


def _bridge_error_to_mcp(message: dict[str, Any], exc: _HttpError) -> dict[str, Any]:
    """Surface 4xx from the bridge as a non-fatal MCP tool result.

    We deliberately do NOT use a JSON-RPC error here: the bridge's 4xx
    responses are recoverable (they include validNodeNames /
    wrapperIdSuggestions / etc.) and the model needs to see the structured
    fields, not a flat error string. ``isError: True`` lets the agent
    framework still treat it as a failed call.
    """
    detail = exc.payload.get("detail") if isinstance(exc.payload, dict) else exc.payload
    text = json.dumps(
        {"success": False, "httpStatus": exc.status, "detail": detail},
        sort_keys=False,
    )
    return {
        "jsonrpc": "2.0",
        "id": message.get("id"),
        "result": {
            "content": [{"type": "text", "text": text}],
            "isError": True,
        },
    }


_HANDLERS = {
    "initialize": _handle_initialize,
    "tools/list": _handle_tools_list,
    "tools/call": _handle_tools_call,
    # MCP also defines notifications/initialized and ping; both are no-ops here.
}


def _handle(message: dict[str, Any]) -> dict[str, Any] | None:
    method = message.get("method")
    if method in ("notifications/initialized", "ping"):
        # Notifications get no response; ping has no useful payload from us.
        return None
    handler = _HANDLERS.get(method)
    if handler is None:
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {"code": -32601, "message": f"Method not found: {method}"},
        }
    try:
        return handler(message)
    except Exception as exc:  # noqa: BLE001 — top-level safety net
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {"code": -32000, "message": f"{type(exc).__name__}: {exc}"},
        }


def main() -> None:
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError as exc:
            error = {
                "jsonrpc": "2.0",
                "error": {"code": -32700, "message": f"Parse error: {exc}"},
                "id": None,
            }
            sys.stdout.write(json.dumps(error) + "\n")
            sys.stdout.flush()
            continue
        response = _handle(message)
        if response is not None:
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
