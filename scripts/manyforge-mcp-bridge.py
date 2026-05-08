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
    MANYFORGE_ALLOWED_TOOLS   — optional comma-separated request-scoped tool ids.
                                When set, tools/list exposes only those ids plus
                                the helper ids below, intersected with the mode
                                allowlist. tools/call is rejected outside the
                                same filtered surface.
    MANYFORGE_ALLOWED_TOOLS_FILE — optional file containing the same comma-
                                separated list. Default:
                                /tmp/manyforge-openclaw-allowed-tools.txt.
                                This is used by the OpenClaw adapter because
                                OpenClaw MCP server env is static in config.
    MANYFORGE_HELPER_TOOLS    — comma-separated safe helper ids auto-exposed
                                when MANYFORGE_ALLOWED_TOOLS is set.
                                default: catalog_read,program_read,scene_inspect,
                                         deployment_capabilities_read,skills_read,
                                         status_read,program_validate

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

# NOTE: Earlier attempt added `host.openshell.internal` to NO_PROXY to
# bypass the SSRF guard on GET requests. That broke the wrapper
# entirely because the only network path to Composer from inside the
# sandbox runs through the proxy at 10.200.0.1:3128 — bypassing it
# yields "Connection refused". Keep the proxy. State-snapshot fetches
# for the delta block must use endpoints the proxy allows (POST to
# /api/assistant/bridge/* is allowed; arbitrary GETs may not be).
ASSISTANT_MODE = os.environ.get("MANYFORGE_ASSISTANT_MODE", "composer-assistant")
PRINCIPAL = os.environ.get("MANYFORGE_PRINCIPAL", "openclaw-sandbox")
REQUEST_TIMEOUT = float(os.environ.get("MANYFORGE_REQUEST_TIMEOUT", "120"))
_ALLOWED_TOOLS_FILE = os.environ.get(
    "MANYFORGE_ALLOWED_TOOLS_FILE",
    "/tmp/manyforge-openclaw-allowed-tools.txt",
)
_RAW_ALLOWED_TOOLS = os.environ.get("MANYFORGE_ALLOWED_TOOLS", "")
if os.path.exists(_ALLOWED_TOOLS_FILE):
    try:
        _RAW_ALLOWED_TOOLS = open(_ALLOWED_TOOLS_FILE, encoding="utf-8").read()
    except OSError:
        pass
REQUEST_ALLOWED_TOOLS = {
    item
    for item in (part.strip() for part in _RAW_ALLOWED_TOOLS.split(","))
    if item
}
DEFAULT_HELPER_TOOLS = (
    "catalog_read,program_read,scene_inspect,deployment_capabilities_read,"
    "skills_read,status_read,program_validate"
)
REQUEST_HELPER_TOOLS = {
    item
    for item in (part.strip() for part in os.environ.get("MANYFORGE_HELPER_TOOLS", DEFAULT_HELPER_TOOLS).split(","))
    if item
}

_CA_BUNDLE = os.environ.get("MANYFORGE_CA_BUNDLE", "/etc/openshell-tls/ca-bundle.pem")
_SSL_CTX: ssl.SSLContext | None = None

# Stable per-process identity. OpenClaw spawns one bridge per agent
# session, so this maps cleanly to "one conversation".
CONVERSATION_ID = f"openclaw-{uuid.uuid4().hex[:12]}"

# Cached mode manifest — fetched lazily on first tools/list, refreshed on
# 409 catalog-hash mismatches (deployment hot-reload window).
_MODE_MANIFEST: dict[str, Any] | None = None

# Optional JSONL audit log of tool calls. Each tools/call appends one
# record: {ts, tool, requestId, success, args}. Smoke harnesses read
# this to inspect "what tool the model actually called per prompt".
# Empty path disables. This is observability only — the wrapper does
# not derive any control-flow state from it.
_DELTA_LOG_PATH = os.environ.get(
    "MANYFORGE_DELTA_LOG", "/tmp/manyforge-delta.log"
).strip()


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


def _mode_tool_entries(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        tool
        for tool in (manifest.get("tools") or [])
        if isinstance(tool, dict) and isinstance(tool.get("id"), str) and tool.get("id")
    ]


def _visible_tool_entries(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    tools = _mode_tool_entries(manifest)
    if not REQUEST_ALLOWED_TOOLS:
        return tools

    mode_ids = {str(tool["id"]) for tool in tools}
    visible_ids = (REQUEST_ALLOWED_TOOLS | REQUEST_HELPER_TOOLS) & mode_ids
    return [tool for tool in tools if str(tool.get("id")) in visible_ids]


def _visible_tool_ids(manifest: dict[str, Any]) -> set[str]:
    return {str(tool["id"]) for tool in _visible_tool_entries(manifest)}


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
                "requestScopedTools": sorted(REQUEST_ALLOWED_TOOLS),
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
    tools = [_mcp_tool_descriptor(t) for t in _visible_tool_entries(manifest)]
    return {
        "jsonrpc": "2.0",
        "id": message.get("id"),
        "result": {"tools": tools},
    }


def _normalize_tool_id(name: str, allowed_ids: set[str]) -> str | None:
    """Resolve a model-emitted tool name to its canonical manifest id.

    OpenClaw's bundle-mcp registers each Composer tool under a
    provider-safe alias by stripping the dots from our canonical ids
    (``scene_draft_add_object`` becomes ``manyforge__scene-draft-add_object``
    in the model's tool list). The gateway is supposed to reverse that
    when it forwards ``tools/call`` to this wrapper, but in practice
    the model occasionally emits the prefixed/dashed form directly,
    or a typo'd dot/underscore variant.

    Without this normalizer, those calls fall into the "tool not
    available" error path and the model — taking the rejection at
    face value — gives up. We want the wrapper to be permissive about
    name shape: try the call name verbatim first, then strip any
    ``manyforge__`` prefix, then try a separator-insensitive lookup
    against the manifest. Returns the canonical manifest id when one
    of the variants matches, ``None`` if none do.
    """
    if name in allowed_ids:
        return name
    # Strip the OpenClaw bundle-mcp prefix if the model included it.
    stripped = name
    for prefix in ("manyforge__", "manyforge-", "manyforge_", "manyforge."):
        if stripped.startswith(prefix):
            stripped = stripped[len(prefix):]
            break
    if stripped in allowed_ids:
        return stripped

    # Separator-insensitive lookup: collapse non-alphanumerics and
    # compare. Catches every dot/dash/underscore mix the model might
    # emit (`scene_draft_add_object`, `scene-draft-add_object`,
    # `scene_draft_add_object`) without enumerating each pair.
    def _key(s: str) -> str:
        return "".join(ch if ch.isalnum() else "" for ch in s).lower()

    target_key = _key(stripped)
    if not target_key:
        return None
    for candidate in allowed_ids:
        if _key(candidate) == target_key:
            return candidate
    return None


def _log_tool_event(
    *,
    tool_id: str,
    request_id: str,
    success: bool,
    user_args: dict[str, Any] | None = None,
) -> None:
    """Append one JSONL record per tools/call for observability.

    Records {ts, tool, requestId, success, args}. Used by smoke
    harnesses to inspect what the model invoked per prompt. Empty
    ``MANYFORGE_DELTA_LOG`` disables. Never raises — logging is
    advisory; failure here must not break the call.
    """
    if not _DELTA_LOG_PATH:
        return
    try:
        record = {
            "ts": time.time(),
            "tool": tool_id,
            "requestId": request_id,
            "success": success,
        }
        if isinstance(user_args, dict):
            # Trim large dict/list values to keep the log readable.
            # Full args are still in Composer's bridge audit.
            record["args"] = {
                k: v for k, v in user_args.items()
                if not isinstance(v, (dict, list)) or len(json.dumps(v)) < 400
            }
        with open(_DELTA_LOG_PATH, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(record, sort_keys=False) + "\n")
    except Exception:
        pass


def _handle_tools_call(message: dict[str, Any]) -> dict[str, Any]:
    params = message.get("params") or {}
    raw_tool_id = params.get("name")
    raw_arguments = params.get("arguments")
    arguments = raw_arguments if isinstance(raw_arguments, dict) else {}
    if not isinstance(raw_tool_id, str) or not raw_tool_id:
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {"code": -32602, "message": "tools/call requires a tool name"},
        }


    manifest = _get_mode_manifest()
    allowed_ids = _visible_tool_ids(manifest)
    canonical_tool_id = _normalize_tool_id(raw_tool_id, allowed_ids)
    if canonical_tool_id is None:
        # Hard-stop client-side too. The bridge endpoint will reject it,
        # but failing here gives the model a clearer, smaller error. When a
        # request-scoped filter is active, this also keeps OpenClaw from
        # wandering into unrelated mode tools.
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {
                "code": -32601,
                "message": (
                    f"tool '{raw_tool_id}' is not exposed by this request's "
                    f"assistant tool window for mode '{ASSISTANT_MODE}'"
                ),
                "data": {"availableTools": sorted(allowed_ids)},
            },
        }
    tool_id = canonical_tool_id

    # 2026-05-06: pre-validate required parameters before forwarding to
    # the bridge. Tracked down via OpenClaw transcript inspection: when
    # the model emits `input: null` (or omits required keys) the previous
    # behaviour was to coerce to {} and let the backend respond silently
    # with an empty result, giving the model no error signal and causing
    # multi-hundred-turn loops. Returning a structured isError:True
    # result with the missing-fields list lets the model recover on the
    # next turn the same way it does on the direct lane.
    tool_entry = next((t for t in _visible_tool_entries(manifest) if str(t.get("id")) == tool_id), None)
    schema = (tool_entry or {}).get("inputSchema") or {}
    required_fields = schema.get("required") if isinstance(schema, dict) else None
    if isinstance(required_fields, list) and required_fields:
        missing = [k for k in required_fields if not (isinstance(arguments, dict) and k in arguments)]
        if raw_arguments is None or missing:
            schema_examples = schema.get("examples") if isinstance(schema, dict) else None
            err_text = json.dumps(
                {
                    "success": False,
                    "error": "missing_required_parameters",
                    "tool": tool_id,
                    "missing": missing or list(required_fields),
                    "required": list(required_fields),
                    "examples": schema_examples or [],
                    "hint": (
                        "Call this tool again with the missing parameters in the "
                        "`arguments` object. Do not call it with no arguments."
                    ),
                },
                sort_keys=False,
            )
            return {
                "jsonrpc": "2.0",
                "id": message.get("id"),
                "result": {
                    "content": [{"type": "text", "text": err_text}],
                    "isError": True,
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

    result_content: dict[str, Any] = {
        "success": success,
        "result": payload,
        "warnings": payload.get("warnings") or body.get("warnings") or [],
        "error": error_text,
        "draftMutated": body.get("draftMutated", False),
        "requestId": request_id,
    }
    # Observability: log what the model actually called per turn. The
    # native pattern relies on the model reading the tool's `result`
    # payload (which already carries its mutation outcome) and using
    # `program_read` / `scene_inspect` for state refresh when needed.
    _log_tool_event(
        tool_id=tool_id,
        request_id=request_id,
        success=success,
        user_args=arguments,
    )

    content_text = json.dumps(result_content, sort_keys=False)
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
