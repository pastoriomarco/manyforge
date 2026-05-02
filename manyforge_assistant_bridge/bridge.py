"""ManyForge assistant-provider bridge service.

Receives `manyforge.assistant.provider_request.v0` envelopes with the v0.1
`schemaVersion`, `assistantMode`, and embedded mode catalog fields. The bridge
enforces the bounded catalog, runs a small multi-turn loop against an
OpenAI-compatible upstream model, and executes only callback-safe tool effects
through ManyForge callbacks.

Architecture: see `manyforge_specs/docs/spec/485-assistant-bridge-architecture.md`.
Wire contract: see `manyforge_specs/docs/reference/ASSISTANT_BACKEND_CONTRACT.md`.
Mode/catalog spec: see `manyforge_specs/docs/spec/480-assistant-modes-and-bounded-autonomy.md`.
"""

from __future__ import annotations

import copy
import hashlib
import json
import logging
import os
import re
import time
import uuid
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from urllib.parse import quote

import httpx
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

UPSTREAM_BASE_URL = os.environ.get(
    "BRIDGE_UPSTREAM_BASE_URL", "http://127.0.0.1:8000/v1"
)
MANYFORGE_BASE_URL = os.environ.get(
    "BRIDGE_MANYFORGE_BASE_URL", "http://127.0.0.1:9000"
)
BRIDGE_HOST = os.environ.get("BRIDGE_HOST", "127.0.0.1")
BRIDGE_PORT = int(os.environ.get("BRIDGE_PORT", "8100"))
AUDIT_LOG_PATH = Path(
    os.environ.get(
        "BRIDGE_AUDIT_LOG",
        str(Path(__file__).resolve().parent / "audit.log"),
    )
)
REQUEST_TIMEOUT_S = float(os.environ.get("BRIDGE_REQUEST_TIMEOUT_S", "60"))
CALLBACK_TIMEOUT_S = float(os.environ.get("BRIDGE_CALLBACK_TIMEOUT_S", "10"))
MAX_TURNS = max(1, int(os.environ.get("BRIDGE_MAX_TURNS", "4")))
MAX_WALL_TIME_S = float(os.environ.get("BRIDGE_MAX_WALL_TIME_S", str(REQUEST_TIMEOUT_S)))
UPSTREAM_MAX_TOKENS = max(1, int(os.environ.get("BRIDGE_UPSTREAM_MAX_TOKENS", "512")))
UPSTREAM_TEMPERATURE = float(os.environ.get("BRIDGE_UPSTREAM_TEMPERATURE", "0.0"))
UPSTREAM_TOP_P_RAW = os.environ.get("BRIDGE_UPSTREAM_TOP_P", "").strip()
UPSTREAM_TOP_P = float(UPSTREAM_TOP_P_RAW) if UPSTREAM_TOP_P_RAW else None
UPSTREAM_TOP_K_RAW = os.environ.get("BRIDGE_UPSTREAM_TOP_K", "").strip()
UPSTREAM_TOP_K = int(UPSTREAM_TOP_K_RAW) if UPSTREAM_TOP_K_RAW else None
UPSTREAM_ENABLE_THINKING_RAW = os.environ.get("BRIDGE_UPSTREAM_ENABLE_THINKING", "").strip()
UPSTREAM_THINKING_TOKEN_BUDGET_RAW = os.environ.get(
    "BRIDGE_UPSTREAM_THINKING_TOKEN_BUDGET",
    "",
).strip()
UPSTREAM_THINKING_TOKEN_BUDGET = (
    int(UPSTREAM_THINKING_TOKEN_BUDGET_RAW)
    if UPSTREAM_THINKING_TOKEN_BUDGET_RAW
    else None
)

# Model selection: explicit env var preferred; auto-discovery from upstream
# as fallback. The bridge does not read the deployment artifact; ManyForge
# composes the catalog and the bridge enforces it.
EXPLICIT_MODEL = os.environ.get("BRIDGE_MODEL", "").strip() or None
_AUTO_DISCOVERED_MODEL: str | None = None

SUPPORTED_SCHEMA_VERSION_MIN = (0, 1, 0)
SUPPORTED_VERSION_FAMILY = "manyforge.assistant.provider_request.v0"
READ_ONLY_EFFECT = "read_only"
PROPOSAL_EFFECT = "proposal"
COMPOSER_DRAFT_MUTATING_EFFECT = "composer_draft_mutating"
RUNTIME_MUTATING_EFFECT = "runtime_mutating"

_CANCELLED_REQUESTS: set[str] = set()

logger = logging.getLogger("manyforge-assistant-bridge")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")


@dataclass(frozen=True)
class ToolBindings:
    """Bridge-local mapping between ManyForge tool ids and model function names."""

    openai_tools: list[dict[str, Any]]
    alias_to_id: dict[str, str]
    alias_by_id: dict[str, str]
    tool_by_id: dict[str, dict[str, Any]]


@dataclass(frozen=True)
class ClassifiedCall:
    """A model tool call classified against the active ManyForge catalog."""

    raw_call: dict[str, Any]
    canonical_call: dict[str, Any]
    canonical_name: str
    model_name: str
    effect: str
    arguments: dict[str, Any]


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _parse_semver(value: str) -> tuple[int, int, int]:
    """Parse 'major.minor.patch' semver. Tolerant of pre-release/build tags."""
    head = value.split("-", 1)[0].split("+", 1)[0]
    parts = head.split(".")
    if len(parts) < 3:
        raise ValueError(f"invalid schemaVersion {value!r}")
    return (int(parts[0]), int(parts[1]), int(parts[2]))


def _compute_catalog_hash(catalog: dict[str, list[str]], mode: str) -> str:
    """Deterministic SHA-256 over the resolved catalog and active mode."""
    payload = {
        "mode": mode,
        "skills": sorted(catalog.get("skills", []) or []),
        "tools": sorted(catalog.get("tools", []) or []),
        "nodes": sorted(catalog.get("nodes", []) or []),
    }
    blob = json.dumps(payload, separators=(",", ":"), sort_keys=True).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


def _audit_record(record: dict[str, Any]) -> None:
    """Append-only JSONL audit log."""
    AUDIT_LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with AUDIT_LOG_PATH.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(record, separators=(",", ":")) + "\n")


def _request_id_from_payload(payload: dict[str, Any]) -> str:
    candidate = payload.get("requestId") or payload.get("providerRequestId")
    if isinstance(candidate, str) and candidate.strip():
        return candidate.strip()
    return f"br-{uuid.uuid4().hex[:12]}"


def _envelope_error(
    *,
    request_id: str,
    code: str,
    detail: str,
    http_status: int = 400,
) -> JSONResponse:
    """Structured rejection envelope ManyForge can surface to the operator."""
    body = {
        "version": SUPPORTED_VERSION_FAMILY,
        "schemaVersion": "0.1.0",
        "requestId": request_id,
        "error": {"code": code, "detail": detail},
        "message": None,
        "toolCalls": [],
        "proposals": [],
        "warnings": [detail],
        "mutated": False,
        "draftMutated": False,
        "requiresReview": True,
    }
    return JSONResponse(status_code=http_status, content=body)


def _raise_if_cancelled(request_id: str) -> None:
    if request_id in _CANCELLED_REQUESTS:
        _CANCELLED_REQUESTS.discard(request_id)
        raise BridgeCancelled(f"request {request_id} was cancelled")


class BridgeCancelled(Exception):
    """Raised when a request was cancelled through the bridge endpoint."""


# ---------------------------------------------------------------------------
# Model resolution
# ---------------------------------------------------------------------------


async def _resolve_model() -> str | None:
    """Resolve the model identifier to send to the upstream endpoint."""
    global _AUTO_DISCOVERED_MODEL
    if EXPLICIT_MODEL:
        return EXPLICIT_MODEL
    if _AUTO_DISCOVERED_MODEL:
        return _AUTO_DISCOVERED_MODEL

    url = f"{UPSTREAM_BASE_URL.rstrip('/')}/models"
    try:
        async with httpx.AsyncClient(timeout=5.0) as client:
            resp = await client.get(url)
            resp.raise_for_status()
            data = resp.json()
        models = data.get("data") or []
        if models and isinstance(models, list):
            mid = models[0].get("id")
            if isinstance(mid, str) and mid.strip():
                _AUTO_DISCOVERED_MODEL = mid.strip()
                logger.info("auto-discovered upstream model: %s", _AUTO_DISCOVERED_MODEL)
                return _AUTO_DISCOVERED_MODEL
    except Exception as exc:
        logger.warning("model auto-discovery failed: %r", exc)
    return None


# ---------------------------------------------------------------------------
# Envelope validation (per manyforge_specs spec 480 §6.3)
# ---------------------------------------------------------------------------


def _validate_envelope(payload: dict[str, Any]) -> tuple[str, dict[str, list[str]]]:
    """Returns (assistantMode, resolved_catalog). Raises ValueError on failure."""
    schema_version = payload.get("schemaVersion")
    if not isinstance(schema_version, str) or not schema_version.strip():
        raise ValueError("missing or blank schemaVersion (v0.1+ required)")

    try:
        sv = _parse_semver(schema_version)
    except ValueError as exc:
        raise ValueError(f"malformed schemaVersion {schema_version!r}: {exc}") from exc
    if sv < SUPPORTED_SCHEMA_VERSION_MIN:
        raise ValueError(
            f"schemaVersion {schema_version} is below the required minimum "
            f"{'.'.join(str(p) for p in SUPPORTED_SCHEMA_VERSION_MIN)}; "
            "assistantMode-aware envelopes only"
        )

    assistant_mode = payload.get("assistantMode")
    if not isinstance(assistant_mode, str) or not assistant_mode.strip():
        raise ValueError(
            "missing or blank assistantMode field; v0.1 envelopes MUST carry "
            "an explicit mode identifier (per spec 480 §6.3)"
        )
    assistant_mode = assistant_mode.strip()

    catalog = payload.get("catalog")
    if not isinstance(catalog, dict):
        raise ValueError(
            "missing or malformed catalog field; v0.1 envelopes MUST embed a "
            "{skills, tools, nodes} catalog block"
        )

    resolved: dict[str, list[str]] = {}
    for label in ("skills", "tools", "nodes"):
        value = catalog.get(label)
        if not isinstance(value, list):
            raise ValueError(f"catalog.{label} must be a list of strings")
        entries: list[str] = []
        for entry in value:
            if not isinstance(entry, str) or not entry.strip():
                raise ValueError(f"catalog.{label} entries must be non-empty strings")
            entries.append(entry.strip())
        resolved[label] = entries

    if assistant_mode == "query":
        if resolved["skills"]:
            raise ValueError("query mode catalog.skills MUST be empty (per spec 480 §5.3)")
        if resolved["nodes"]:
            raise ValueError("query mode catalog.nodes MUST be empty (per spec 480 §5.3)")

    return assistant_mode, resolved


# ---------------------------------------------------------------------------
# Tool catalog -> OpenAI tools[] conversion
# ---------------------------------------------------------------------------


def _safe_model_tool_name(tool_id: str, used: set[str]) -> str:
    """Map arbitrary ManyForge tool ids to OpenAI-compatible function names."""
    base = re.sub(r"[^A-Za-z0-9_-]", "_", tool_id.strip())
    base = re.sub(r"_+", "_", base).strip("_") or "tool"
    if not re.match(r"^[A-Za-z_]", base):
        base = f"tool_{base}"
    base = base[:64]
    if base not in used:
        used.add(base)
        return base

    suffix = hashlib.sha256(tool_id.encode("utf-8")).hexdigest()[:8]
    alias = f"{base[:55]}_{suffix}"
    counter = 2
    while alias in used:
        alias = f"{base[:52]}_{suffix}_{counter}"
        counter += 1
    used.add(alias)
    return alias


def _optional_bool_from_env(raw: str) -> bool | None:
    if not raw:
        return None
    normalized = raw.strip().lower()
    if normalized in {"1", "true", "yes", "on"}:
        return True
    if normalized in {"0", "false", "no", "off"}:
        return False
    raise ValueError(
        "BRIDGE_UPSTREAM_ENABLE_THINKING must be one of: "
        "1, true, yes, on, 0, false, no, off"
    )


def _manyforge_tool_to_openai(
    tool: dict[str, Any],
    *,
    model_name: str,
) -> dict[str, Any] | None:
    """Convert a ManyForge tool descriptor to OpenAI function-tool shape."""
    if not isinstance(tool, dict):
        return None
    tool_id = tool.get("id") or tool.get("name")
    if not isinstance(tool_id, str) or not tool_id.strip():
        return None
    description = tool.get("description") or ""
    if not isinstance(description, str):
        description = ""

    return {
        "type": "function",
        "function": {
            "name": model_name,
            "description": description,
            "parameters": _tool_parameters_schema(tool),
        },
    }


_JSON_SCHEMA_MARKER_KEYS = {
    "$schema",
    "$defs",
    "additionalProperties",
    "allOf",
    "anyOf",
    "oneOf",
    "properties",
    "required",
    "type",
}


def _tool_parameters_schema(tool: dict[str, Any]) -> dict[str, Any]:
    """Return the OpenAI function parameters schema for a ManyForge tool."""
    for key in ("parameters", "inputSchema", "input_schema"):
        raw_schema = tool.get(key)
        if isinstance(raw_schema, dict) and (
            _JSON_SCHEMA_MARKER_KEYS.intersection(raw_schema)
        ):
            schema = copy.deepcopy(raw_schema)
            schema.setdefault("type", "object")
            schema.setdefault("properties", {})
            return schema
    return {
        "type": "object",
        "properties": {},
        "additionalProperties": True,
    }


def _tool_effect(tool: dict[str, Any]) -> str:
    effect = tool.get("effect")
    if isinstance(effect, str) and effect.strip():
        return effect.strip()
    return PROPOSAL_EFFECT


def _build_tool_bindings(
    request_tools: list[dict[str, Any]] | None,
    allowed_tool_ids: set[str],
) -> ToolBindings:
    """Filter ManyForge tool descriptors to the resolved catalog and convert."""
    if not request_tools:
        return ToolBindings([], {}, {}, {})

    openai_tools: list[dict[str, Any]] = []
    alias_to_id: dict[str, str] = {}
    alias_by_id: dict[str, str] = {}
    tool_by_id: dict[str, dict[str, Any]] = {}
    used_aliases: set[str] = set()

    for entry in request_tools:
        if not isinstance(entry, dict):
            continue
        tool_id = entry.get("id") or entry.get("name")
        if not isinstance(tool_id, str):
            continue
        tool_id = tool_id.strip()
        if tool_id not in allowed_tool_ids:
            continue

        alias = _safe_model_tool_name(tool_id, used_aliases)
        oai = _manyforge_tool_to_openai(entry, model_name=alias)
        if oai is None:
            continue
        openai_tools.append(oai)
        alias_to_id[alias] = tool_id
        alias_by_id[tool_id] = alias
        tool_by_id[tool_id] = dict(entry)

    return ToolBindings(openai_tools, alias_to_id, alias_by_id, tool_by_id)


# ---------------------------------------------------------------------------
# Upstream LLM dispatch and ManyForge callback execution
# ---------------------------------------------------------------------------


async def _call_upstream(
    *,
    model: str,
    messages: list[dict[str, Any]],
    tools: list[dict[str, Any]],
    timeout_s: float,
) -> dict[str, Any]:
    """Single chat-completion call to the upstream OpenAI-compatible endpoint."""
    body: dict[str, Any] = {
        "model": model,
        "messages": messages,
        "temperature": UPSTREAM_TEMPERATURE,
        "max_tokens": UPSTREAM_MAX_TOKENS,
    }
    if UPSTREAM_TOP_P is not None:
        body["top_p"] = UPSTREAM_TOP_P
    if UPSTREAM_TOP_K is not None:
        body["top_k"] = UPSTREAM_TOP_K
    chat_template_kwargs: dict[str, Any] = {}
    enable_thinking = _optional_bool_from_env(UPSTREAM_ENABLE_THINKING_RAW)
    if enable_thinking is not None:
        chat_template_kwargs["enable_thinking"] = enable_thinking
    if UPSTREAM_THINKING_TOKEN_BUDGET is not None:
        chat_template_kwargs["thinking_token_budget"] = UPSTREAM_THINKING_TOKEN_BUDGET
    if chat_template_kwargs:
        body["chat_template_kwargs"] = chat_template_kwargs
    if tools:
        body["tools"] = tools
        body["tool_choice"] = "auto"

    url = f"{UPSTREAM_BASE_URL.rstrip('/')}/chat/completions"
    async with httpx.AsyncClient(timeout=timeout_s) as client:
        resp = await client.post(url, json=body)
        resp.raise_for_status()
        return resp.json()


async def _execute_callback_tool(
    *,
    request_id: str,
    conversation_id: str | None,
    assistant_mode: str,
    catalog_hash: str,
    tool_id: str,
    arguments: dict[str, Any],
    effect: str,
) -> dict[str, Any]:
    """Execute one callback-safe tool through ManyForge's callback surface."""
    url = (
        f"{MANYFORGE_BASE_URL.rstrip('/')}/api/assistant/bridge/tools/"
        f"{quote(tool_id, safe='')}"
    )
    body = {
        "requestId": request_id,
        "conversationId": conversation_id,
        "assistantMode": assistant_mode,
        "catalogHash": catalog_hash,
        "arguments": arguments,
    }
    try:
        async with httpx.AsyncClient(timeout=CALLBACK_TIMEOUT_S) as client:
            resp = await client.post(url, json=body)
    except httpx.TimeoutException:
        return {
            "success": False,
            "error": f"callback for {tool_id} timed out after {CALLBACK_TIMEOUT_S}s",
            "mutated": False,
            "draftMutated": False,
        }
    except Exception as exc:
        return {
            "success": False,
            "error": f"callback for {tool_id} failed: {exc!r}",
            "mutated": False,
            "draftMutated": False,
        }

    if resp.status_code >= 400:
        return {
            "success": False,
            "error": f"ManyForge callback returned HTTP {resp.status_code}: {resp.text[:300]}",
            "mutated": False,
            "draftMutated": False,
        }

    try:
        data = resp.json()
    except Exception as exc:
        return {
            "success": False,
            "error": f"ManyForge callback returned non-JSON response: {exc!r}",
            "mutated": False,
            "draftMutated": False,
        }
    if not isinstance(data, dict):
        return {
            "success": False,
            "error": "ManyForge callback response must be a JSON object",
            "mutated": False,
            "draftMutated": False,
        }
    if data.get("mutated") is True and effect != COMPOSER_DRAFT_MUTATING_EFFECT:
        return {
            "success": False,
            "error": f"ManyForge callback for {tool_id} reported mutation; rejected",
            "mutated": False,
            "draftMutated": False,
        }
    if effect == COMPOSER_DRAFT_MUTATING_EFFECT:
        data["draftMutated"] = bool(data.get("mutated") or data.get("draftMutated"))
    else:
        data["draftMutated"] = False
    data["mutated"] = False
    return data


async def _execute_read_only_tool(
    **kwargs: Any,
) -> dict[str, Any]:
    """Compatibility wrapper for tests and older imports."""
    return await _execute_callback_tool(effect=READ_ONLY_EFFECT, **kwargs)


# ---------------------------------------------------------------------------
# Response shaping (upstream -> ManyForge envelope)
# ---------------------------------------------------------------------------


def _parse_args(raw: Any) -> dict[str, Any]:
    if isinstance(raw, str):
        try:
            parsed = json.loads(raw)
            return parsed if isinstance(parsed, dict) else {"value": parsed}
        except json.JSONDecodeError:
            return {"_raw": raw}
    if isinstance(raw, dict):
        return raw
    return {}


def _hash_arguments(arguments: dict[str, Any]) -> str:
    return hashlib.sha256(
        json.dumps(arguments, sort_keys=True, default=str).encode("utf-8")
    ).hexdigest()[:16]


def _canonicalize_tool_call(call: dict[str, Any], canonical_name: str) -> dict[str, Any]:
    canonical = dict(call)
    fn = dict(canonical.get("function") or {})
    fn["name"] = canonical_name
    canonical["function"] = fn
    return canonical


def _shape_proposal_from_tool_call(call: dict[str, Any]) -> dict[str, Any]:
    fn = call.get("function") or {}
    name = fn.get("name") or "unknown_tool"
    args_obj = _parse_args(fn.get("arguments"))
    return {
        "id": call.get("id") or f"prop-{uuid.uuid4().hex[:12]}",
        "type": "tool_call_proposal",
        "title": f"call {name}",
        "summary": f"LLM proposes calling {name} with the supplied arguments.",
        "payload": {
            "toolName": name,
            "arguments": args_obj,
            "toolCallId": call.get("id"),
        },
    }


def _shape_tool_call_record(
    call: dict[str, Any],
    *,
    status: str = "proposed",
    result: dict[str, Any] | None = None,
    error: str | None = None,
) -> dict[str, Any]:
    fn = call.get("function") or {}
    name = fn.get("name") or "unknown_tool"
    args_obj = _parse_args(fn.get("arguments"))
    record: dict[str, Any] = {"name": name, "status": status, "arguments": args_obj}
    if result is not None:
        record["result"] = result
    if error is not None:
        record["error"] = error
    return record


def _extract_upstream_message(
    upstream_response: dict[str, Any],
) -> tuple[str, list[dict[str, Any]], dict[str, Any]]:
    choices = upstream_response.get("choices") or []
    if not choices:
        return ("", [], {"role": "assistant", "content": ""})
    msg = choices[0].get("message") or {}
    content = msg.get("content") or ""
    if not isinstance(content, str):
        content = ""
    raw_tool_calls = msg.get("tool_calls") or []
    if not isinstance(raw_tool_calls, list):
        raw_tool_calls = []

    assistant_msg: dict[str, Any] = {"role": "assistant", "content": content}
    if raw_tool_calls:
        assistant_msg["tool_calls"] = raw_tool_calls
    return content, raw_tool_calls, assistant_msg


def _classify_response(
    *,
    upstream_response: dict[str, Any],
    bindings: ToolBindings,
) -> tuple[str, dict[str, Any], list[ClassifiedCall], list[dict[str, Any]], list[str]]:
    """Inspect the upstream response and partition tool_calls.

    Returns:
      (message_text, assistant_message, accepted_calls, rejected_meta, names)
    """
    content, raw_tool_calls, assistant_msg = _extract_upstream_message(upstream_response)
    accepted: list[ClassifiedCall] = []
    rejected_meta: list[dict[str, Any]] = []
    out_of_catalog_names: list[str] = []

    for call in raw_tool_calls:
        if not isinstance(call, dict):
            out_of_catalog_names.append("<malformed>")
            rejected_meta.append({"name": "<malformed>", "argumentsHash": ""})
            continue
        fn = call.get("function") or {}
        name = (fn.get("name") or "").strip()
        arguments = _parse_args(fn.get("arguments"))
        canonical_name = bindings.alias_to_id.get(name)
        if canonical_name is None:
            out_of_catalog_names.append(name or "<unnamed>")
            rejected_meta.append(
                {
                    "name": name or "<unnamed>",
                    "argumentsHash": _hash_arguments(arguments),
                }
            )
            continue

        tool = bindings.tool_by_id.get(canonical_name, {})
        accepted.append(
            ClassifiedCall(
                raw_call=call,
                canonical_call=_canonicalize_tool_call(call, canonical_name),
                canonical_name=canonical_name,
                model_name=name,
                effect=_tool_effect(tool),
                arguments=arguments,
            )
        )

    return content, assistant_msg, accepted, rejected_meta, out_of_catalog_names


def _tool_result_message(
    *,
    call: ClassifiedCall,
    result: dict[str, Any],
) -> dict[str, Any]:
    return {
        "role": "tool",
        "tool_call_id": call.raw_call.get("id") or f"call-{uuid.uuid4().hex[:12]}",
        "name": call.model_name,
        "content": json.dumps(result, separators=(",", ":"), default=str),
    }


def _success_envelope(
    *,
    request_id: str,
    message_text: str,
    proposals: list[dict[str, Any]],
    tool_calls: list[dict[str, Any]],
    warnings: list[str] | None = None,
    draft_mutated: bool = False,
) -> dict[str, Any]:
    message = message_text.strip() or "Assistant completed without a text response."
    return {
        "version": SUPPORTED_VERSION_FAMILY,
        "schemaVersion": "0.1.0",
        "requestId": request_id,
        "message": message,
        "toolCalls": tool_calls,
        "proposals": proposals,
        "warnings": list(warnings or []),
        "mutated": False,
        "draftMutated": draft_mutated,
        "requiresReview": True,
    }


# ---------------------------------------------------------------------------
# FastAPI app
# ---------------------------------------------------------------------------


@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info(
        "ManyForge assistant bridge starting on %s:%s; upstream %s; manyforge %s; "
        "audit %s; max turns %s; explicit model: %s",
        BRIDGE_HOST,
        BRIDGE_PORT,
        UPSTREAM_BASE_URL,
        MANYFORGE_BASE_URL,
        AUDIT_LOG_PATH,
        MAX_TURNS,
        EXPLICIT_MODEL or "(auto-discover)",
    )
    yield
    logger.info("ManyForge assistant bridge shutting down")


app = FastAPI(
    title="ManyForge assistant-provider bridge",
    version="0.1.0",
    lifespan=lifespan,
)


@app.get("/healthz")
async def healthz() -> dict[str, Any]:
    model = await _resolve_model()
    return {
        "status": "ok",
        "version": SUPPORTED_VERSION_FAMILY,
        "schemaVersion": "0.1.0",
        "upstreamBaseUrl": UPSTREAM_BASE_URL,
        "manyforgeBaseUrl": MANYFORGE_BASE_URL,
        "model": model,
        "modelSource": (
            "explicit" if EXPLICIT_MODEL else ("auto" if _AUTO_DISCOVERED_MODEL else "unresolved")
        ),
        "maxTurns": MAX_TURNS,
        "maxWallTimeS": MAX_WALL_TIME_S,
    }


@app.post("/v1/manyforge/assistant/{request_id}/cancel")
async def cancel_request(request_id: str) -> dict[str, Any]:
    request_id = request_id.strip()
    if not request_id:
        return {"cancelled": False, "detail": "request_id is blank"}
    _CANCELLED_REQUESTS.add(request_id)
    _audit_record(
        {
            "ts": time.time(),
            "requestId": request_id,
            "exitReason": "cancel_requested",
        }
    )
    return {"requestId": request_id, "cancelled": True}


@app.post("/v1/manyforge/assistant")
async def assistant(request: Request) -> JSONResponse:
    started_ns = time.perf_counter_ns()

    try:
        payload = await request.json()
    except Exception as exc:
        return _envelope_error(
            request_id=f"br-{uuid.uuid4().hex[:12]}",
            code="invalid_json",
            detail=f"request body is not valid JSON: {exc}",
            http_status=400,
        )

    if not isinstance(payload, dict):
        return _envelope_error(
            request_id=f"br-{uuid.uuid4().hex[:12]}",
            code="invalid_envelope",
            detail="request body must be a JSON object",
            http_status=400,
        )

    request_id = _request_id_from_payload(payload)
    conversation_id = payload.get("conversationId")
    if not isinstance(conversation_id, str):
        conversation_id = None

    try:
        _raise_if_cancelled(request_id)
    except BridgeCancelled as exc:
        _audit_record(
            {
                "ts": time.time(),
                "requestId": request_id,
                "exitReason": "cancelled",
                "detail": str(exc),
            }
        )
        return _envelope_error(
            request_id=request_id,
            code="cancelled",
            detail=str(exc),
            http_status=200,
        )

    try:
        assistant_mode, catalog = _validate_envelope(payload)
    except ValueError as exc:
        _audit_record(
            {
                "ts": time.time(),
                "requestId": request_id,
                "exitReason": "envelope_rejected",
                "detail": str(exc),
            }
        )
        return _envelope_error(
            request_id=request_id,
            code="envelope_rejected",
            detail=str(exc),
            http_status=400,
        )

    catalog_hash = _compute_catalog_hash(catalog, assistant_mode)
    allowed_tool_ids = set(catalog["tools"])

    model = await _resolve_model()
    if not model:
        _audit_record(
            {
                "ts": time.time(),
                "requestId": request_id,
                "assistantMode": assistant_mode,
                "catalogHash": catalog_hash,
                "exitReason": "model_unresolved",
            }
        )
        return _envelope_error(
            request_id=request_id,
            code="model_unresolved",
            detail=(
                "bridge could not resolve a model identifier; set BRIDGE_MODEL "
                "env var or ensure the upstream endpoint serves at least one model"
            ),
            http_status=503,
        )

    user_message = payload.get("message") or ""
    if not isinstance(user_message, str):
        user_message = json.dumps(user_message)
    messages = [{"role": "user", "content": user_message}]

    request_tools = payload.get("tools") or []
    if not isinstance(request_tools, list):
        request_tools = []
    bindings = _build_tool_bindings(request_tools, allowed_tool_ids)

    completed_callback_calls: list[dict[str, Any]] = []
    draft_mutated = False
    warnings: list[str] = []

    try:
        for turn_index in range(1, MAX_TURNS + 1):
            _raise_if_cancelled(request_id)
            elapsed_s = (time.perf_counter_ns() - started_ns) / 1_000_000_000
            remaining_s = MAX_WALL_TIME_S - elapsed_s
            if remaining_s <= 0:
                raise BridgeTimeout(f"bridge loop exceeded {MAX_WALL_TIME_S}s")
            timeout_s = max(0.1, min(REQUEST_TIMEOUT_S, remaining_s))

            try:
                upstream_resp = await _call_upstream(
                    model=model,
                    messages=messages,
                    tools=bindings.openai_tools,
                    timeout_s=timeout_s,
                )
            except httpx.HTTPStatusError as exc:
                detail = f"upstream HTTP {exc.response.status_code}: {exc.response.text[:300]}"
                _audit_record(
                    {
                        "ts": time.time(),
                        "requestId": request_id,
                        "assistantMode": assistant_mode,
                        "catalogHash": catalog_hash,
                        "model": model,
                        "turnCount": turn_index,
                        "exitReason": "upstream_http_error",
                        "detail": detail,
                    }
                )
                return _envelope_error(
                    request_id=request_id,
                    code="upstream_http_error",
                    detail=detail,
                    http_status=502,
                )
            except httpx.TimeoutException:
                raise BridgeTimeout(f"upstream request exceeded {timeout_s:.1f}s") from None
            except Exception as exc:
                _audit_record(
                    {
                        "ts": time.time(),
                        "requestId": request_id,
                        "assistantMode": assistant_mode,
                        "catalogHash": catalog_hash,
                        "model": model,
                        "turnCount": turn_index,
                        "exitReason": "upstream_call_error",
                        "detail": repr(exc),
                    }
                )
                return _envelope_error(
                    request_id=request_id,
                    code="upstream_call_error",
                    detail=repr(exc),
                    http_status=502,
                )

            (
                message_text,
                assistant_msg,
                accepted_calls,
                rejected_meta,
                out_of_catalog_names,
            ) = _classify_response(upstream_response=upstream_resp, bindings=bindings)

            duration_ms = (time.perf_counter_ns() - started_ns) / 1_000_000

            if out_of_catalog_names:
                detail = (
                    "upstream model emitted tool calls outside the active "
                    f"assistantMode '{assistant_mode}' catalog: "
                    + ", ".join(sorted(set(out_of_catalog_names)))
                )
                _audit_record(
                    {
                        "ts": time.time(),
                        "requestId": request_id,
                        "assistantMode": assistant_mode,
                        "catalogHash": catalog_hash,
                        "model": model,
                        "outOfMode": rejected_meta,
                        "turnCount": turn_index,
                        "durationMs": round(duration_ms, 1),
                        "exitReason": "response_rejected",
                        "detail": detail,
                    }
                )
                return _envelope_error(
                    request_id=request_id,
                    code="response_rejected",
                    detail=detail,
                    http_status=200,
                )

            callback_calls: list[ClassifiedCall] = []
            invalid_callback_calls: list[ClassifiedCall] = []
            for call in accepted_calls:
                if call.effect == READ_ONLY_EFFECT:
                    callback_calls.append(call)
                elif call.effect == COMPOSER_DRAFT_MUTATING_EFFECT:
                    if assistant_mode == "composer-assistant":
                        callback_calls.append(call)
                    else:
                        invalid_callback_calls.append(call)

            if invalid_callback_calls:
                detail = (
                    "composer_draft_mutating tools may only execute in "
                    "assistantMode 'composer-assistant': "
                    + ", ".join(call.canonical_name for call in invalid_callback_calls)
                )
                _audit_record(
                    {
                        "ts": time.time(),
                        "requestId": request_id,
                        "assistantMode": assistant_mode,
                        "catalogHash": catalog_hash,
                        "model": model,
                        "turnCount": turn_index,
                        "durationMs": round(duration_ms, 1),
                        "exitReason": "response_rejected",
                        "detail": detail,
                    }
                )
                return _envelope_error(
                    request_id=request_id,
                    code="response_rejected",
                    detail=detail,
                    http_status=200,
                )

            descriptor_calls = [
                call for call in accepted_calls if call not in callback_calls
            ]

            if descriptor_calls:
                if callback_calls:
                    warnings.append(
                        "model returned callback-safe calls together with proposal/runtime "
                        "descriptors; descriptor response ended the loop"
                    )
                proposals = [
                    _shape_proposal_from_tool_call(call.canonical_call)
                    for call in descriptor_calls
                ]
                tool_calls = completed_callback_calls + [
                    _shape_tool_call_record(call.canonical_call)
                    for call in descriptor_calls
                ]
                _audit_record(
                    {
                        "ts": time.time(),
                        "requestId": request_id,
                        "assistantMode": assistant_mode,
                        "catalogHash": catalog_hash,
                        "model": model,
                        "responseShape": {
                            "toolCallNames": [call["name"] for call in tool_calls],
                            "proposalCount": len(proposals),
                            "warnings": len(warnings),
                            "draftMutated": draft_mutated,
                        },
                        "turnCount": turn_index,
                        "durationMs": round(duration_ms, 1),
                        "exitReason": "proposal_returned",
                    }
                )
                return JSONResponse(
                    status_code=200,
                    content=_success_envelope(
                        request_id=request_id,
                        message_text=message_text,
                        proposals=proposals,
                        tool_calls=tool_calls,
                        warnings=warnings,
                        draft_mutated=draft_mutated,
                    ),
                )

            if not callback_calls:
                _audit_record(
                    {
                        "ts": time.time(),
                        "requestId": request_id,
                        "assistantMode": assistant_mode,
                        "catalogHash": catalog_hash,
                        "model": model,
                        "responseShape": {
                            "toolCallNames": [call["name"] for call in completed_callback_calls],
                            "proposalCount": 0,
                            "warnings": len(warnings),
                            "draftMutated": draft_mutated,
                        },
                        "turnCount": turn_index,
                        "durationMs": round(duration_ms, 1),
                        "exitReason": "ok",
                    }
                )
                return JSONResponse(
                    status_code=200,
                    content=_success_envelope(
                        request_id=request_id,
                        message_text=message_text,
                        proposals=[],
                        tool_calls=completed_callback_calls,
                        warnings=warnings,
                        draft_mutated=draft_mutated,
                    ),
                )

            if turn_index >= MAX_TURNS:
                detail = (
                    f"bridge reached maxTurns={MAX_TURNS} while the model still "
                    "requested callback tools"
                )
                _audit_record(
                    {
                        "ts": time.time(),
                        "requestId": request_id,
                        "assistantMode": assistant_mode,
                        "catalogHash": catalog_hash,
                        "model": model,
                        "turnCount": turn_index,
                        "durationMs": round(duration_ms, 1),
                        "exitReason": "max_turns_exceeded",
                        "detail": detail,
                    }
                )
                return _envelope_error(
                    request_id=request_id,
                    code="max_turns_exceeded",
                    detail=detail,
                    http_status=200,
                )

            messages.append(assistant_msg)
            for call in callback_calls:
                _raise_if_cancelled(request_id)
                result = await _execute_callback_tool(
                    request_id=request_id,
                    conversation_id=conversation_id,
                    assistant_mode=assistant_mode,
                    catalog_hash=catalog_hash,
                    tool_id=call.canonical_name,
                    arguments=call.arguments,
                    effect=call.effect,
                )
                if result.get("draftMutated") is True:
                    draft_mutated = True
                completed_callback_calls.append(
                    _shape_tool_call_record(
                        call.canonical_call,
                        status="completed" if result.get("success") is not False else "failed",
                        result=result.get("result") if isinstance(result.get("result"), dict) else result,
                        error=result.get("error") if isinstance(result.get("error"), str) else None,
                    )
                )
                messages.append(_tool_result_message(call=call, result=result))

    except BridgeCancelled as exc:
        _audit_record(
            {
                "ts": time.time(),
                "requestId": request_id,
                "assistantMode": locals().get("assistant_mode"),
                "catalogHash": locals().get("catalog_hash"),
                "model": locals().get("model"),
                "exitReason": "cancelled",
                "detail": str(exc),
            }
        )
        return _envelope_error(
            request_id=request_id,
            code="cancelled",
            detail=str(exc),
            http_status=200,
        )
    except BridgeTimeout as exc:
        _audit_record(
            {
                "ts": time.time(),
                "requestId": request_id,
                "assistantMode": locals().get("assistant_mode"),
                "catalogHash": locals().get("catalog_hash"),
                "model": locals().get("model"),
                "exitReason": "timeout",
                "detail": str(exc),
            }
        )
        return _envelope_error(
            request_id=request_id,
            code="timeout",
            detail=str(exc),
            http_status=504,
        )

    return _envelope_error(
        request_id=request_id,
        code="internal_loop_error",
        detail="bridge loop exited unexpectedly",
        http_status=500,
    )


class BridgeTimeout(Exception):
    """Raised when the bridge loop exceeds its time budget."""


# ---------------------------------------------------------------------------
# CLI entry
# ---------------------------------------------------------------------------


def main() -> None:
    import uvicorn

    uvicorn.run(
        "manyforge_assistant_bridge.bridge:app",
        host=BRIDGE_HOST,
        port=BRIDGE_PORT,
        log_level="info",
    )


if __name__ == "__main__":
    main()
