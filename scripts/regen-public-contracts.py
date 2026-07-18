#!/usr/bin/env python3
"""Regenerate operator-facing public contract docs in `docs/reference/`.

Produces three markdown files derived from code-as-source-of-truth:

  - docs/reference/REST_API.md
        Composer REST surface, generated from the FastAPI app's
        OpenAPI schema (manyforge_composer.backend.app:app).

  - docs/reference/MCP_RUNTIME_CONTRACT.md
        ManyForge MCP tools (names, descriptions, input schemas),
        generated from the MCP server module
        (manyforge_composer.backend.mcp_server).

  - docs/reference/ASSISTANT_PROVIDER_CONTRACT.md
        Assistant-provider HTTP wire format, generated from the bridge
        FastAPI app's OpenAPI schema (manyforge_assistant_bridge.bridge:app)
        plus the envelope-validation contract documented in protocol.py.

Why this script exists:
  These docs replace links to the private `manyforge_specs` repo so
  deployment users have everything they need on disk. Hand-copying the
  spec content would introduce a second source of truth and drift.
  Generation from code keeps the deployment-side docs synchronised with
  the actual runtime code without manual reconciliation.

Usage:
    cd $REPO_ROOT
    python3 scripts/regen-public-contracts.py

The script imports manyforge modules; run it from a Python environment
that can import them (the standard Composer dev container, or the host
venv used for the bridge).

When extending: add new generators that read code-level facts (decorator
metadata, type hints, FastAPI routes, schemas) and produce concise
operator-facing markdown. NEVER copy normative content from
manyforge_specs into the generator's output.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
import textwrap
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
REF_DIR = REPO_ROOT / "docs" / "reference"
DEFAULT_CATALOG_PATH = str(
    REPO_ROOT / "manyforge_behavior" / "resources" / "node_catalog.yaml"
)

# The repository is a mixed colcon/Python workspace: Composer is importable
# from the repository root, while manyforge_behavior uses a nested package
# root. Make the documented direct invocation deterministic instead of relying
# on an ambient PYTHONPATH from a particular test or launch wrapper.
for source_root in (REPO_ROOT, REPO_ROOT / "manyforge_behavior"):
    source_text = str(source_root)
    if source_text not in sys.path:
        sys.path.insert(0, source_text)


def _build_composer_state():
    """Construct a minimal ComposerState suitable for OpenAPI / MCP introspection.

    The factories `create_app(state)` and `create_mcp_server(state)` both
    require a real `ComposerState` instance. We use the same `MockKernelClient`
    that the test suite uses — the kernel is never invoked here; we only
    introspect the FastAPI/MCP shape after construction.
    """
    from manyforge_behavior.kernel_client import MockKernelClient
    from manyforge_behavior.node_environment import build_node_environment
    from manyforge_composer.backend.state import ComposerState
    return ComposerState(
        node_environment=build_node_environment(DEFAULT_CATALOG_PATH),
        kernel=MockKernelClient(),
    )
HEADER_TEMPLATE = (
    "<!--\n"
    "  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.\n"
    "  Regenerate with:\n"
    "    cd {repo_root}\n"
    "    python3 scripts/regen-public-contracts.py\n"
    "  Last regenerated: {timestamp}\n"
    "  Generator source: scripts/regen-public-contracts.py:{generator_name}\n"
    "-->\n\n"
)


def _stamp(generator_name: str) -> str:
    return HEADER_TEMPLATE.format(
        repo_root=str(REPO_ROOT),
        timestamp=time.strftime("%Y-%m-%d %H:%M:%S %Z"),
        generator_name=generator_name,
    )


# `--check` mode compares freshly-generated content to what's on disk. The
# embedded `Last regenerated: <now>` timestamp would otherwise always
# differ, defeating the check. Normalize the timestamp line on both sides
# before comparing.
_TIMESTAMP_LINE_RE = re.compile(r"  Last regenerated: .*\n")


def _strip_timestamp(content: str) -> str:
    return _TIMESTAMP_LINE_RE.sub("  Last regenerated: <normalized>\n", content)


# ---------------------------------------------------------------------------
# REST API generator
# ---------------------------------------------------------------------------

def gen_rest_api() -> str:
    """Render the Composer REST surface from its FastAPI OpenAPI schema."""
    try:
        from manyforge_composer.backend.app import create_app
        state = _build_composer_state()
        app = create_app(state)
        spec = app.openapi()
    except Exception as exc:
        return _stamp("gen_rest_api") + textwrap.dedent(f"""
            # Composer REST API

            **AUTO-GENERATION FAILED** — could not import the Composer
            FastAPI app at module level. This is expected when the
            `manyforge_composer` package is not importable from the env
            running this script. Run from inside the dev container:

                docker compose run --rm dev bash
                python3 scripts/regen-public-contracts.py

            Error captured: `{type(exc).__name__}: {exc}`

            ## Live source of truth

            Run Composer and visit:

            - `http://localhost:9000/openapi.json` — raw OpenAPI 3.0 schema
            - `http://localhost:9000/docs` — interactive Swagger UI
            - `http://localhost:9000/redoc` — ReDoc-rendered reference

            ## Code source

            FastAPI app construction:
            `manyforge_composer/backend/app.py:build_app()`

            Per-router files under `manyforge_composer/backend/routes_*.py`.
        """).lstrip()

    body: list[str] = [_stamp("gen_rest_api"), "# Composer REST API\n"]
    body.append(
        f"FastAPI app, served by `python -m manyforge_composer ...` on the "
        f"port specified by `--port` (default `9000`). The live schema is "
        f"available at `/openapi.json` and `/docs` while Composer is "
        f"running.\n\n"
    )
    body.append(f"OpenAPI version: `{spec.get('openapi', '?')}`. "
                f"Title: `{spec.get('info', {}).get('title', '?')}`.\n\n")
    body.append("## Routes\n\n")
    paths = spec.get("paths", {})
    rows = []
    for path, methods in sorted(paths.items()):
        for method, op in sorted((methods or {}).items()):
            if method.upper() not in ("GET", "POST", "PUT", "DELETE", "PATCH"):
                continue
            summary = (op.get("summary") or op.get("description") or "").splitlines()
            summary_text = summary[0] if summary else ""
            tag = ", ".join(op.get("tags") or [])
            rows.append((method.upper(), path, tag, summary_text))
    body.append("| Method | Path | Tag | Summary |\n|---|---|---|---|\n")
    for method, path, tag, summary in rows:
        body.append(f"| `{method}` | `{path}` | {tag} | {summary} |\n")
    return "".join(body)


# ---------------------------------------------------------------------------
# MCP runtime contract generator
# ---------------------------------------------------------------------------

def gen_mcp_contract() -> str:
    """Render the ManyForge MCP tool surface."""
    try:
        from manyforge_composer.backend.mcp_server import create_mcp_server
        state = _build_composer_state()
        server = create_mcp_server(state)
        # FastMCP exposes the registered tools through its tool manager.
        # Layout has shifted across versions, so try a couple of shapes.
        tool_manager = getattr(server, "_tool_manager", None)
        tools = {}
        if tool_manager is not None:
            tools = getattr(tool_manager, "_tools", None) or {}
            if not tools:
                tools = getattr(tool_manager, "tools", None) or {}
        if not tools:
            tools = getattr(server, "_tools", None) or {}
        if not tools:
            raise RuntimeError("MCP server has no registered tools")
    except Exception as exc:
        return _stamp("gen_mcp_contract") + textwrap.dedent(f"""
            # ManyForge MCP Runtime Contract

            **AUTO-GENERATION FAILED** — could not introspect the MCP
            server module. Error: `{type(exc).__name__}: {exc}`.

            ## Live source of truth

            Start the MCP HTTP shim and query the tools/list endpoint:

                python -m manyforge_composer --mcp-http \\
                  --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml

            Then:

                curl -s -X POST http://localhost:9000/api/mcp \\
                  -H 'Content-Type: application/json' \\
                  -d '{{"method":"tools/list","jsonrpc":"2.0","id":1}}' \\
                  | python3 -m json.tool

            ## Code source

            Tool registrations: `manyforge_composer/backend/mcp_server.py`
            (each `@mcp.tool()` decorator registers one tool).
        """).lstrip()

    body: list[str] = [_stamp("gen_mcp_contract"), "# ManyForge MCP Runtime Contract\n"]
    body.append(
        "ManyForge ships an MCP (Model Context Protocol) server that exposes "
        "Composer's program/scene/tree/blackboard tools to LLM agents over a "
        "stdio transport. Agents can `tools/list` to discover the surface and "
        "`tools/call` to invoke individual tools. The server enforces a "
        "deployment-scoped catalog so only mode-allowed tools are reachable.\n\n"
    )
    body.append(f"Tool count: **{len(tools)}**.\n\n")
    body.append("## Tools (machine-introspected at regen time)\n\n")
    for name, tool in sorted(tools.items()):
        desc = (getattr(tool, "description", "") or "").strip()
        first_line = desc.split("\n", 1)[0]
        body.append(f"### `{name}`\n\n")
        body.append(f"{first_line}\n\n")
        # Show the input schema if available.
        schema = getattr(tool, "parameters", None) or getattr(tool, "input_schema", None)
        if schema:
            try:
                body.append("Input schema:\n```json\n")
                body.append(json.dumps(schema, indent=2, default=str))
                body.append("\n```\n\n")
            except Exception:
                pass
    body.append("## Code source\n\n"
                "All tools are registered with `@mcp.tool()` decorators in "
                "`manyforge_composer/backend/mcp_server.py`. The decorator "
                "captures the tool's name, description (from the function's "
                "docstring), and JSON-Schema input shape from the function's "
                "type hints.\n")
    return "".join(body)


# ---------------------------------------------------------------------------
# Assistant provider contract generator
# ---------------------------------------------------------------------------

def gen_assistant_provider_contract() -> str:
    """Render the assistant-provider HTTP wire format."""
    try:
        from manyforge_assistant_bridge import bridge as bridge_module
        spec = bridge_module.app.openapi()
    except Exception as exc:
        spec = None
        err = exc

    out: list[str] = [_stamp("gen_assistant_provider_contract"),
                      "# Assistant Provider Contract\n"]
    out.append(
        "Composer dispatches assistant requests to a *provider* HTTP service "
        "which speaks the `manyforge.assistant.provider_request.v0` envelope. "
        "Two reference providers ship with manyforge:\n\n"
        "- `nemoclaw` lane: `manyforge_assistant_bridge` on port 8100 — "
        "  runs the agent loop in-process; calls the model directly. "
        "  Same repo as this doc.\n"
        "- `openclaw` lane: `openclaw_assistant_bridge` on port 8200 — "
        "  dispatches into a NemoClaw sandbox running OpenClaw. Lives in "
        "  the platform repo (NemoClaw-Thor) under `manyforge/`.\n\n"
        "Both speak the same wire envelope; this doc covers the wire format "
        "the provider must accept.\n\n"
    )
    if spec:
        out.append("## Bridge HTTP routes (machine-introspected)\n\n")
        out.append("| Method | Path | Summary |\n|---|---|---|\n")
        for path, methods in sorted(spec.get("paths", {}).items()):
            for method, op in sorted((methods or {}).items()):
                if method.upper() not in ("GET", "POST", "PUT", "DELETE", "PATCH"):
                    continue
                summary = (op.get("summary") or "").splitlines()
                summary_text = summary[0] if summary else ""
                out.append(f"| `{method.upper()}` | `{path}` | {summary_text} |\n")
        out.append("\n")
    else:
        out.append(f"_Auto-introspection failed: `{type(err).__name__}: {err}`._\n\n")

    out.append(
        "## Envelope wire format\n\n"
        "The request body sent by Composer to the provider on "
        "`POST /v1/manyforge/assistant`:\n\n"
        "```json\n"
        "{\n"
        '  "version": "manyforge.assistant.provider_request.v0",\n'
        '  "schemaVersion": "0.1.0",\n'
        '  "requestId": "<uuid>",\n'
        '  "conversationId": "<uuid>",\n'
        '  "principal": "<operator id>",\n'
        '  "assistantMode": "composer-assistant" | "query" | ...,\n'
        '  "message": "<user prompt>",\n'
        '  "tools": [ {"id": "...", "description": "...", "parameters": {...}}, ... ],\n'
        '  "constraints": { "mutatesState": bool, "requiresReview": bool, "proposalStatus": "draft", ... },\n'
        '  "context": { "catalogHash": "<sha256>", "programRevision": "...", ... },\n'
        '  "programSnapshot": { ... },\n'
        '  "sceneSnapshot": { ... },\n'
        '  "nodeCatalog": [ ... ],\n'
        '  "skillCatalog": [ ... ]\n'
        "}\n"
        "```\n\n"
        "## Catalog-hash gate\n\n"
        "Every envelope carries a `context.catalogHash` computed by the "
        "shared protocol helper. Any provider implementation must validate "
        "the hash before executing tool calls; mismatch → HTTP 409 with "
        "`code: catalog_mismatch`. Hash function lives in:\n\n"
        "  `manyforge_assistant_bridge/protocol.py:compute_catalog_hash`\n\n"
        "Both the Composer manifest emitter and the bridge consumer compute "
        "the hash from the same canonical input shape, so byte-identical "
        "output is required across the wire.\n\n"
        "## Provider response\n\n"
        "On success, the provider returns:\n\n"
        "```json\n"
        "{\n"
        '  "requestId": "<echo>",\n'
        '  "conversationId": "<echo>",\n'
        '  "providerId": "nemoclaw" | "openclaw" | "...",\n'
        '  "message": "<final assistant text>",\n'
        '  "proposals": [ ... ],          // draft mutations the operator must accept\n'
        '  "toolCalls": [ ... ],          // executed callbacks (audit trail)\n'
        '  "warnings": [ ... ],\n'
        '  "draftMutated": bool,\n'
        '  "openclaw": { ... }            // optional provider-specific telemetry\n'
        "}\n"
        "```\n\n"
        "Errors return non-200 with:\n\n"
        "```json\n"
        "{ \"requestId\": \"<echo>\", \"code\": \"<short_id>\", \"detail\": \"<human-readable>\" }\n"
        "```\n\n"
        "## Code source\n\n"
        "- `manyforge_assistant_bridge/bridge.py` — direct lane provider (this repo).\n"
        "- `manyforge_assistant_bridge/protocol.py` — shared hashing helpers.\n"
        "- `manyforge_composer/backend/routes_assistant.py` — Composer-side "
        "  envelope assembly + tool-callback endpoints (`/api/assistant/chat`, "
        "  `/api/assistant/bridge/tools/<tool_id>`).\n"
    )
    return "".join(out)


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--check", action="store_true",
                        help="Exit 1 if regen would change any file (CI mode).")
    args = parser.parse_args()

    REF_DIR.mkdir(parents=True, exist_ok=True)
    targets = {
        "REST_API.md": gen_rest_api,
        "MCP_RUNTIME_CONTRACT.md": gen_mcp_contract,
        "ASSISTANT_PROVIDER_CONTRACT.md": gen_assistant_provider_contract,
    }
    drift = []
    for fname, generator in targets.items():
        path = REF_DIR / fname
        new_content = generator()
        old_content = path.read_text() if path.exists() else ""
        if args.check:
            if _strip_timestamp(new_content) != _strip_timestamp(old_content):
                drift.append(fname)
        else:
            path.write_text(new_content)
            print(f"  wrote {path.relative_to(REPO_ROOT)} ({len(new_content)} chars)")
    if args.check and drift:
        print("ERROR: regen would change:", ", ".join(drift), file=sys.stderr)
        print("Run scripts/regen-public-contracts.py to update.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
