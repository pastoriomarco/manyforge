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
    MANYFORGE_LANE            — lane identifier; used to derive default
                                principal and ID prefixes for
                                conversation/request audit.
                                default: openclaw (preserves pre-lane-aware
                                deployments bit-identical)
    MANYFORGE_COMPOSER_BASE   — Composer HTTP root.
                                default: http://host.openshell.internal:9000
    MANYFORGE_ASSISTANT_MODE  — assistant mode to expose.
                                default: composer-assistant
    MANYFORGE_PRINCIPAL       — principal name recorded in audit.
                                default: <LANE>-sandbox
    MANYFORGE_REQUEST_TIMEOUT — per-HTTP-call timeout in seconds.
                                default: 120
    MANYFORGE_CA_BUNDLE       — TLS CA bundle path.
                                default: /etc/openshell-tls/ca-bundle.pem if present

Usage (per lane):
  OpenClaw — emit into openclaw.json mcpServers:
    {"mcpServers": {"manyforge": {"command": "python3",
        "args": ["/sandbox/.manyforge/manyforge-mcp-bridge.py"],
        "env": {"MANYFORGE_LANE": "openclaw"}}}}

  Hermes — emit into Hermes config.yaml mcp_servers:
    mcp_servers:
      manyforge:
        command: python3
        args: ["/sandbox/manyforge/scripts/manyforge-mcp-bridge.py"]
        env:
          MANYFORGE_LANE: "hermes"
          MANYFORGE_ASSISTANT_MODE: "composer-assistant"
          MANYFORGE_COMPOSER_BASE: "http://host.openshell.internal:9000"
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


# Lane identifier — derives default principal and audit ID prefixes.
# Defaults to "openclaw" so deployments that pre-date the lane-aware
# refactor are bit-identical to the old behavior.
# Sanitized via .strip() or "openclaw" so an empty env doesn't corrupt
# IDs/log columns.
LANE = (os.environ.get("MANYFORGE_LANE", "openclaw").strip() or "openclaw")

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
PRINCIPAL = os.environ.get("MANYFORGE_PRINCIPAL", f"{LANE}-sandbox")
REQUEST_TIMEOUT = float(os.environ.get("MANYFORGE_REQUEST_TIMEOUT", "120"))

_CA_BUNDLE = os.environ.get("MANYFORGE_CA_BUNDLE", "/etc/openshell-tls/ca-bundle.pem")
_SSL_CTX: ssl.SSLContext | None = None

# Stable per-process identity. OpenClaw/Hermes spawns one bridge per
# agent session, so this maps cleanly to "one conversation". The
# LANE prefix lets cross-lane audit reports attribute each row to a lane
# at a glance.
CONVERSATION_ID = f"{LANE}-{uuid.uuid4().hex[:12]}"

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
    _trace_event("http_start", method=method, path=path, timeout=REQUEST_TIMEOUT)
    t0 = time.time()
    try:
        resp = urllib.request.urlopen(req, timeout=REQUEST_TIMEOUT, context=ctx)
        raw = resp.read()
        _trace_event("http_recv", method=method, path=path,
                     status=resp.status, elapsed=time.time() - t0, bytes=len(raw))
        parsed = json.loads(raw)
        _trace_event("http_done", method=method, path=path,
                     elapsed=time.time() - t0)
        return parsed
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        _trace_event("http_httperror", method=method, path=path,
                     status=exc.code, elapsed=time.time() - t0)
        try:
            payload = json.loads(raw)
        except (ValueError, TypeError):
            payload = {"detail": raw[:1024]}
        # Re-raise as a typed exception so call sites can branch on status.
        raise _HttpError(exc.code, payload) from exc
    except Exception as exc:
        _trace_event("http_exc", method=method, path=path,
                     elapsed=time.time() - t0,
                     exc_type=type(exc).__name__, exc_msg=str(exc)[:200])
        raise


class _HttpError(Exception):
    def __init__(self, status: int, payload: dict[str, Any]) -> None:
        super().__init__(f"HTTP {status}: {payload}")
        self.status = status
        self.payload = payload


def _refresh_mode_manifest() -> dict[str, Any]:
    """Fetch the mode-scoped tool/node manifest from Composer."""
    global _MODE_MANIFEST
    _trace_event("manifest_fetch_start", mode=ASSISTANT_MODE,
                 composer_base=COMPOSER_BASE)
    manifest = _http("GET", f"/api/assistant/modes/{ASSISTANT_MODE}")
    _trace_event("manifest_fetch_done", mode=ASSISTANT_MODE,
                 tools_count=len(manifest.get("tools") or []),
                 catalog_hash=manifest.get("catalogHash"))
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
    return _mode_tool_entries(manifest)


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
                "toolSurfaceScope": "assistant_mode",
            },
        },
    }


def _handle_tools_list(message: dict[str, Any]) -> dict[str, Any]:
    _trace_event("tools_list_enter", id=message.get("id"))
    try:
        manifest = _get_mode_manifest()
    except _HttpError as exc:
        _trace_event("tools_list_http_err", id=message.get("id"),
                     status=exc.status)
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
    except Exception as exc:
        _trace_event("tools_list_exc", id=message.get("id"),
                     exc_type=type(exc).__name__, exc_msg=str(exc)[:200])
        raise
    _trace_event("tools_list_post_manifest", id=message.get("id"),
                 visible_count_pre_filter=len(manifest.get("tools") or []))
    visible = _visible_tool_entries(manifest)
    _trace_event("tools_list_post_visible", id=message.get("id"),
                 visible=len(visible))
    tools = [_mcp_tool_descriptor(t) for t in visible]
    _trace_event("tools_list_post_descriptors", id=message.get("id"),
                 emitted=len(tools))
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

    Records {ts, lane, principal, conversationId, assistantMode, tool,
    requestId, success, args}. Used by smoke harnesses to inspect what
    the model invoked per prompt and by cross-lane compare reports to
    attribute each row to a lane / conversation. Empty
    ``MANYFORGE_DELTA_LOG`` disables. Never raises — logging is
    advisory; failure here must not break the call.

    The lane/principal/conversationId/assistantMode columns let the
    local MCP bridge audit be self-contained for cross-lane debugging
    (without them, you have to cross-reference Composer's bridge-tools
    log to attribute a row to a session).
    """
    if not _DELTA_LOG_PATH:
        return
    try:
        record = {
            "ts": time.time(),
            "lane": LANE,
            "principal": PRINCIPAL,
            "conversationId": CONVERSATION_ID,
            "assistantMode": ASSISTANT_MODE,
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


# 2026-06-08: catalog_read loop-breaker. gemma-class models can fall into a tight
# loop calling `catalog_read` over and over instead of acting on the first result.
# No infra guard catches it — each read is a fast, successful, in-budget call (case
# budget, turn backstop, stale/orphan, and the MCP breaker all key on slowness /
# orphaning / FAILURES, none of which a quick successful re-read trips), so the run
# just burns turns and never acts. We break it at the dispatch layer: on the 2nd+
# CONSECUTIVE `catalog_read` within a burst, short-circuit with a nudge to use the
# catalog already in context. A >30s gap (a new case/turn is ~a minute apart; an
# intra-turn loop is seconds apart) and ANY other tool call both reset the streak,
# so we never false-trip across cases or after real progress.
#
# Deliberately scoped to `catalog_read` ONLY — not other reads and not mutations.
# A bulked request ("add several objects", multi-node authoring) legitimately fires
# the same mutation tool back-to-back, and state reads (program_read/scene_inspect)
# can legitimately recur between edits; catalog_read is reference data that does not
# change within a turn, so a consecutive re-read is always pure waste.
_LOOP_GUARDED_TOOLS = frozenset({"catalog_read"})
_CONSEC_READ_GAP_S = 30.0
_consec_read: dict[str, Any] = {"tool": None, "ts": 0.0, "n": 0}


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
        # but failing here gives the model a clearer, smaller error.
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "error": {
                "code": -32601,
                "message": (
                    f"tool '{raw_tool_id}' is not exposed by assistant "
                    f"mode '{ASSISTANT_MODE}'"
                ),
                "data": {"availableTools": sorted(allowed_ids)},
            },
        }
    tool_id = canonical_tool_id

    # catalog_read loop-breaker (see _LOOP_GUARDED_TOOLS note above). Scoped to
    # catalog_read only; ANY other tool call resets the streak (real progress).
    _now = time.time()
    if tool_id in _LOOP_GUARDED_TOOLS:
        if _consec_read["tool"] == tool_id and (_now - _consec_read["ts"]) <= _CONSEC_READ_GAP_S:
            _consec_read["n"] += 1
        else:
            _consec_read["tool"] = tool_id
            _consec_read["n"] = 1
        _consec_read["ts"] = _now
        if _consec_read["n"] >= 2:
            loop_text = json.dumps({
                "success": False,
                "error": "redundant_catalog_read",
                "tool": tool_id,
                "hint": (
                    "You already called `catalog_read` on the previous turn; the catalog "
                    "(node kinds + parameters) is in the conversation above and does not "
                    "change within a turn. Re-reading makes no progress. Use that catalog "
                    "NOW: call the appropriate draft/mutation tool to act, or emit your "
                    "final answer. Do not call `catalog_read` again this turn."
                ),
            }, sort_keys=False)
            _log_tool_event(tool_id=tool_id,
                            request_id=f"{LANE}-loopbreak-{int(_now * 1000)}",
                            success=False, user_args=arguments)
            return {
                "jsonrpc": "2.0",
                "id": message.get("id"),
                "result": {"content": [{"type": "text", "text": loop_text}], "isError": True},
            }
    else:
        _consec_read["tool"] = None
        _consec_read["n"] = 0

    # 2026-06-08: serve catalog_read LOCALLY from the manifest the bridge already
    # holds (fetched once via GET /api/assistant/modes, which is NOT a counted
    # bridge-tool dispatch), so catalog_read never POSTs to Composer's
    # /api/assistant/bridge/tools and therefore never registers as an "observed
    # tool call" in the corpus scorer. Why this matters: the native lane has no
    # gateway-internal discovery, so the model reads the catalog via catalog_read
    # (a read, not an action) — but as a Composer dispatch it was being counted,
    # unfairly failing "expected NO tool calls" cases that OpenClaw (gateway-internal
    # tool_search/describe, uncounted) and Direct (inline catalog, no read) pass.
    # The mode manifest's nodeCatalog carries the full mode-visible kinds + their
    # parameters (verified: covers repeat/command_gripper/wait_for_signal_bool/the
    # collision family, etc.), so the model still gets everything it needs — just
    # without a counted round-trip, and faster. Read-only; never mutates state.
    if tool_id == "catalog_read":
        _entries = [e for e in (manifest.get("nodeCatalog") or []) if isinstance(e, dict)]
        _ids = arguments.get("entryIds") if isinstance(arguments.get("entryIds"), list) else None
        _kind = arguments.get("kind") if isinstance(arguments.get("kind"), str) else None
        _cat = arguments.get("category") if isinstance(arguments.get("category"), str) else None
        if _ids:
            _entries = [e for e in _entries if e.get("id") in _ids]
        elif _kind:
            _entries = [e for e in _entries if e.get("id") == _kind or e.get("kind") == _kind]
        elif _cat:
            _entries = [e for e in _entries if e.get("category") == _cat]
        if arguments.get("includeParameters") is False:
            _entries = [{k: v for k, v in e.items() if k != "parameters"} for e in _entries]
        _local = {
            "success": True,
            "result": {"entries": _entries, "servedBy": "mcp-bridge-local",
                       "catalogHash": manifest.get("catalogHash")},
            "warnings": [], "error": None, "draftMutated": False,
            "requestId": f"{LANE}-localcat-{int(_now * 1000)}",
        }
        _log_tool_event(tool_id=tool_id, request_id=_local["requestId"],
                        success=True, user_args=arguments)
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "result": {"content": [{"type": "text", "text": json.dumps(_local, sort_keys=False)}],
                       "isError": False},
        }

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
    request_id = f"{LANE}-{int(time.time() * 1000)}-{uuid.uuid4().hex[:8]}"
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
    # MCP also defines notifications/initialized (notification, no response)
    # and ping (request with id, MUST return a result). See _handle below.
}


def _handle(message: dict[str, Any]) -> dict[str, Any] | None:
    method = message.get("method")
    if method == "notifications/initialized":
        # Notifications never get a response per JSON-RPC 2.0 spec.
        return None
    if method == "ping":
        # MCP ping protocol: per
        # https://modelcontextprotocol.io/specification/2024-11-05/basic/utilities/ping
        # the receiver MUST respond promptly with an empty result object.
        # OpenClaw's bundle-mcp StdioClientTransport sends ping after
        # initialize as a liveness check; if we don't respond, the gateway
        # treats the server as unresponsive and emits
        #   McpError: MCP error -32001: Request timed out
        # after its connectionTimeoutMs window — leaving the manyforge
        # catalog unregistered for the entire session and the model
        # seeing only OpenClaw core tools (verified 2026-06-03 on the
        # 35B bake-off run; cosmos sessions had been working by chance
        # because they used a different invocation path that did not
        # trigger ping).
        return {
            "jsonrpc": "2.0",
            "id": message.get("id"),
            "result": {},
        }
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


# Optional MCP request/response trace logger. Set
# MANYFORGE_MCP_TRACE to a writable file path to enable.
# Each line is a single JSON object with { ts, dir, method, id, ... }.
# Dir is "in" for incoming requests, "out" for responses we send,
# "err" for parse/exception failures. Useful for diagnosing
# "Unknown tool id" / "Request timed out" cycles in the gateway log
# — those messages only tell you OpenClaw is unhappy; the trace tells
# you whether the bridge actually received the request and what it
# replied with. Empty / unset = no trace, no overhead.
_TRACE_PATH = os.environ.get("MANYFORGE_MCP_TRACE", "").strip() or None


def _trace(direction: str, payload: dict[str, Any]) -> None:
    if not _TRACE_PATH:
        return
    try:
        rec = {
            "ts": time.time(),
            "dir": direction,
            "method": payload.get("method"),
            "id": payload.get("id"),
            # For request payloads, capture the params; for responses,
            # capture result or error. Keep small to avoid bloating
            # the trace on big tools/list responses.
            "params_keys": sorted((payload.get("params") or {}).keys()) if isinstance(payload.get("params"), dict) else None,
            "has_result": "result" in payload,
            "error_code": (payload.get("error") or {}).get("code") if isinstance(payload.get("error"), dict) else None,
            "lane": LANE,
            "principal": PRINCIPAL,
        }
        with open(_TRACE_PATH, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec) + "\n")
    except Exception:
        # Trace is best-effort; never let it break the real RPC loop.
        pass


def _trace_event(stage: str, **fields: Any) -> None:
    """Emit an ad-hoc trace marker keyed by `stage`. Safe to call from any
    code path; never raises. Used to localize hangs inside request handlers.
    """
    if not _TRACE_PATH:
        return
    try:
        rec = {"ts": time.time(), "dir": "evt", "stage": stage,
               "lane": LANE, "principal": PRINCIPAL}
        rec.update(fields)
        with open(_TRACE_PATH, "a", encoding="utf-8") as fh:
            fh.write(json.dumps(rec) + "\n")
    except Exception:
        pass


def main() -> None:
    # Emit a startup marker so an operator inspecting the trace knows
    # the bridge actually started AND that the env is what we expect.
    if _TRACE_PATH:
        try:
            import socket as _socket
            _dns_t0 = time.time()
            _dns_err = None
            _dns_addrs = None
            try:
                _dns_addrs = [a[4][0] for a in _socket.getaddrinfo("host.openshell.internal", 9000, type=_socket.SOCK_STREAM)]
            except Exception as _dns_e:
                _dns_err = f"{type(_dns_e).__name__}: {_dns_e}"
            _dns_elapsed = time.time() - _dns_t0
            _tcp_t0 = time.time()
            _tcp_err = None
            try:
                _s = _socket.create_connection(("host.openshell.internal", 9000), timeout=5)
                _s.close()
            except Exception as _tcp_e:
                _tcp_err = f"{type(_tcp_e).__name__}: {_tcp_e}"
            _tcp_elapsed = time.time() - _tcp_t0
            with open(_TRACE_PATH, "a", encoding="utf-8") as fh:
                fh.write(json.dumps({
                    "ts": time.time(),
                    "dir": "boot_env",
                    "HTTP_PROXY": os.environ.get("HTTP_PROXY"),
                    "http_proxy": os.environ.get("http_proxy"),
                    "NO_PROXY": os.environ.get("NO_PROXY"),
                    "no_proxy": os.environ.get("no_proxy"),
                    "ALL_PROXY": os.environ.get("ALL_PROXY"),
                    "urllib_proxies": __import__("urllib.request", fromlist=["getproxies"]).getproxies(),
                    "urllib_proxy_bypass": __import__("urllib.request", fromlist=["proxy_bypass"]).proxy_bypass("host.openshell.internal:9000"),
                    "dns_addrs": _dns_addrs,
                    "dns_err": _dns_err,
                    "dns_elapsed": _dns_elapsed,
                    "tcp_err": _tcp_err,
                    "tcp_elapsed": _tcp_elapsed,
                }) + "\n")
        except Exception as _e:
            pass
    if _TRACE_PATH:
        try:
            with open(_TRACE_PATH, "a", encoding="utf-8") as fh:
                fh.write(json.dumps({
                    "ts": time.time(),
                    "dir": "boot",
                    "lane": LANE,
                    "principal": PRINCIPAL,
                    "assistant_mode": ASSISTANT_MODE,
                    "composer_base": os.environ.get("MANYFORGE_COMPOSER_BASE"),
                    "no_proxy_has_host_openshell": "host.openshell.internal"
                        in (os.environ.get("NO_PROXY") or os.environ.get("no_proxy") or ""),
                    "http_proxy_set": bool(os.environ.get("HTTP_PROXY") or os.environ.get("http_proxy")),
                    "pythonunbuffered": os.environ.get("PYTHONUNBUFFERED"),
                }) + "\n")
        except Exception:
            pass
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
            _trace("err", error)
            sys.stdout.write(json.dumps(error) + "\n")
            sys.stdout.flush()
            continue
        _trace("in", message)
        response = _handle(message)
        if response is not None:
            _trace("out", response)
            sys.stdout.write(json.dumps(response) + "\n")
            sys.stdout.flush()


if __name__ == "__main__":
    main()
