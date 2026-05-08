#!/usr/bin/env python3
"""Smoke-test a ManyForge assistant provider endpoint.

The script sends the v0.1 assistant-mode request envelope used by ManyForge's
``NemoClawAssistantProvider`` and performs fail-closed response checks that are
useful before wiring a live bridge into Composer.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


REQUEST_VERSION = "manyforge.assistant.provider_request.v0"
DEFAULT_TOOLS = [
    "inspect_isaac_scene:read_only:Inspect the current simulator scene snapshot",
    "propose_scene_objects:proposal:Propose reviewable scene-resource additions",
    "propose_scene_object_nodes:proposal:Propose reviewable Composer scene nodes",
]
VALID_TOOL_EFFECTS = {"read_only", "proposal", "runtime_mutating"}


def main() -> int:
    args = _parse_args()
    try:
        tools = [_parse_tool_spec(spec) for spec in args.tool]
        tool_ids = [tool["id"] for tool in tools]
        requested_tools = args.requested_tool or list(tool_ids)
        unknown_requested = sorted(set(requested_tools) - set(tool_ids))
        if unknown_requested:
            raise SmokeFailure(
                f"requested tools are outside the visible catalog: {unknown_requested}"
            )
        request_payload = _build_request_payload(
            args=args,
            tools=tools,
            requested_tools=requested_tools,
        )
        response_payload = _post_json(
            args.endpoint,
            request_payload,
            timeout_s=args.timeout_s,
        )
        summary = _validate_response(
            response_payload,
            assistant_mode=args.assistant_mode,
            tool_ids=tool_ids,
            allow_out_of_catalog_tool_calls=args.allow_out_of_catalog_tool_calls,
        )
    except SmokeFailure as exc:
        print(json.dumps({"ok": False, "error": str(exc)}, sort_keys=True), file=sys.stderr)
        return 1

    summary.update(
        {
            "ok": True,
            "endpoint": args.endpoint,
            "assistantMode": args.assistant_mode,
            "requestedTools": requested_tools,
        }
    )
    print(json.dumps(summary, indent=2, sort_keys=True))
    return 0


class SmokeFailure(RuntimeError):
    """Raised when the provider endpoint or response violates the smoke contract."""


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--endpoint",
        default="http://127.0.0.1:8100/v1/manyforge/assistant",
        help="Assistant provider endpoint URL.",
    )
    parser.add_argument("--provider-id", default="nemoclaw")
    parser.add_argument("--conversation-id", default="")
    parser.add_argument("--schema-version", default="0.1.0")
    parser.add_argument("--assistant-mode", default="composer-assistant")
    parser.add_argument(
        "--message",
        default="Create a reviewable scene-resource proposal from the current scene.",
    )
    parser.add_argument(
        "--tool",
        action="append",
        default=[],
        metavar="ID:EFFECT[:DESCRIPTION]",
        help=(
            "Visible tool descriptor. EFFECT must be read_only, proposal, or "
            "runtime_mutating. Defaults to the deterministic scene-authoring tools."
        ),
    )
    parser.add_argument(
        "--requested-tool",
        action="append",
        default=[],
        help="Requested tool ID. Defaults to all visible tool IDs.",
    )
    parser.add_argument("--skill", action="append", default=[], help="Visible skill ID.")
    parser.add_argument("--node", action="append", default=[], help="Visible node ID.")
    parser.add_argument(
        "--source-root",
        action="append",
        default=["/World/soup_can"],
        help="Scene source root to place in request context.",
    )
    parser.add_argument("--timeout-s", type=float, default=10.0)
    parser.add_argument(
        "--allow-out-of-catalog-tool-calls",
        action="store_true",
        help="Do not fail if the response references a tool outside the visible catalog.",
    )
    args = parser.parse_args()
    if args.timeout_s <= 0:
        parser.error("--timeout-s must be > 0")
    if not args.tool:
        args.tool = list(DEFAULT_TOOLS)
    return args


def _parse_tool_spec(spec: str) -> dict[str, Any]:
    parts = spec.split(":", 2)
    if len(parts) < 2:
        raise SmokeFailure(f"tool spec must be ID:EFFECT[:DESCRIPTION], got {spec!r}")
    tool_id = parts[0].strip()
    effect = parts[1].strip()
    description = parts[2].strip() if len(parts) == 3 else ""
    if not tool_id:
        raise SmokeFailure(f"tool spec has empty ID: {spec!r}")
    if effect not in VALID_TOOL_EFFECTS:
        raise SmokeFailure(
            f"tool {tool_id!r} has invalid effect {effect!r}; "
            f"expected one of {sorted(VALID_TOOL_EFFECTS)}"
        )
    return {
        "id": tool_id,
        "name": tool_id,
        "effect": effect,
        "description": description,
    }


def _build_request_payload(
    *,
    args: argparse.Namespace,
    tools: list[dict[str, Any]],
    requested_tools: list[str],
) -> dict[str, Any]:
    conversation_id = args.conversation_id or f"smoke-{time.monotonic_ns()}"
    tool_ids = [tool["id"] for tool in tools]
    return {
        "version": REQUEST_VERSION,
        "schemaVersion": args.schema_version,
        "providerId": args.provider_id,
        "conversationId": conversation_id,
        "assistantMode": args.assistant_mode,
        "message": args.message,
        "requestedTools": requested_tools,
        "context": {
            "sourceRoots": list(args.source_root),
        },
        "runtime": {
            "programLoaded": False,
            "cycleState": "idle",
        },
        # Bare-id `skills`, `nodes`, and `catalog.skills` /
        # `catalog.nodes` / `catalog.tools` were dropped from the
        # envelope contract; consumers derive ids from the rich
        # nodeCatalog/skillCatalog/tools payloads.
        "nodeCatalog": [{"id": node_id} for node_id in args.node],
        "skillCatalog": [{"id": skill_id} for skill_id in args.skill],
        "catalog": {
            "nodeCatalog": [{"id": node_id} for node_id in args.node],
            "skillCatalog": [{"id": skill_id} for skill_id in args.skill],
        },
        "tools": tools,
        "constraints": {
            "mutatesState": False,
            "requiresReview": True,
            "proposalStatus": "draft",
            "allowedToolCallStatuses": [
                "proposed",
                "skipped",
                "completed",
                "failed",
            ],
        },
    }


def _post_json(endpoint: str, payload: dict[str, Any], *, timeout_s: float) -> dict[str, Any]:
    body = json.dumps(payload, sort_keys=True).encode("utf-8")
    request = Request(
        endpoint,
        data=body,
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
        method="POST",
    )
    try:
        with urlopen(request, timeout=timeout_s) as response:
            raw_body = response.read()
            status = response.status
    except HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise SmokeFailure(f"endpoint returned HTTP {exc.code}: {detail}") from exc
    except URLError as exc:
        raise SmokeFailure(f"endpoint request failed: {exc}") from exc
    except TimeoutError as exc:
        raise SmokeFailure(f"endpoint timed out after {timeout_s:.3f}s") from exc

    if status < 200 or status >= 300:
        raise SmokeFailure(f"endpoint returned HTTP {status}: {raw_body!r}")
    try:
        payload = json.loads(raw_body.decode("utf-8"))
    except json.JSONDecodeError as exc:
        raise SmokeFailure("endpoint response was not JSON") from exc
    if not isinstance(payload, dict):
        raise SmokeFailure("endpoint response must be a JSON object")
    return payload


def _validate_response(
    payload: dict[str, Any],
    *,
    assistant_mode: str,
    tool_ids: list[str],
    allow_out_of_catalog_tool_calls: bool,
) -> dict[str, Any]:
    message = payload.get("message", payload.get("response", ""))
    if not isinstance(message, str) or not message.strip():
        raise SmokeFailure("response must include a non-empty message or response string")
    if payload.get("mutated") is True:
        raise SmokeFailure("response must not report state mutation")

    tool_calls = payload.get("toolCalls", payload.get("tool_calls", []))
    proposals = payload.get("proposals", [])
    warnings = payload.get("warnings", [])
    _require_list(tool_calls, "toolCalls")
    _require_list(proposals, "proposals")
    _require_list(warnings, "warnings")

    allowed = set(tool_ids)
    out_of_catalog = sorted(
        {
            str(call.get("name", "")).strip()
            for call in tool_calls
            if isinstance(call, dict)
            and str(call.get("name", "")).strip()
            and str(call.get("name", "")).strip() not in allowed
        }
    )
    if out_of_catalog and not allow_out_of_catalog_tool_calls:
        raise SmokeFailure(
            f"response referenced tools outside assistantMode {assistant_mode!r}: "
            f"{out_of_catalog}"
        )

    smoke_warnings = list(warnings)
    if payload.get("requiresReview") is False:
        smoke_warnings.append(
            "provider response marked requiresReview=false; ManyForge should normalize this"
        )

    return {
        "message": message,
        "toolCallCount": len(tool_calls),
        "proposalCount": len(proposals),
        "warnings": smoke_warnings,
    }


def _require_list(value: Any, field: str) -> None:
    if not isinstance(value, list):
        raise SmokeFailure(f"response.{field} must be a list when present")


if __name__ == "__main__":
    raise SystemExit(main())
