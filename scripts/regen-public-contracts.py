#!/usr/bin/env python3
"""Regenerate code-owned Composer/Headless REST or optional MCP references.

The composable-cell cutover still removes the former aggregate assistant,
viewer, and lifecycle contract owners. The explicit generators are:

  - ``docs/reference/REST_API.md`` from
    ``create_app(ComposerState()).openapi()``.
  - ``docs/reference/HEADLESS_REST_API.md`` from the independent H0/N3 FastAPI
    route owner.
  - ``docs/reference/MCP_RUNTIME_CONTRACT.md`` from the optional stateless
    Composer adapter and the same FastAPI DTO schemas.

Do not add unavailable product surfaces or synthetic compatibility contracts
to this generator. A restored public surface must first regain a real code
owner and complete its plan qualification gate.

Usage:
    cd $REPO_ROOT
    python3 scripts/regen-public-contracts.py structural-rest
    python3 scripts/regen-public-contracts.py headless-rest

The script imports the retained Composer modules; run it from the standard
Composer dev container. It fails closed if the application cannot be imported
or constructed and never writes an error stub in place of a contract.
"""
from __future__ import annotations

import sys
import tempfile


_UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
_LEGACY_AGGREGATE_UNAVAILABLE_MESSAGE = (
    f"{_UNAVAILABLE_REASON_CODE}: aggregate public-contract generation is "
    "unavailable while later runtime contract owners are removed; use one "
    "explicit structural-rest or headless-rest selector."
)


def _is_legacy_aggregate_invocation(arguments: list[str]) -> bool:
    return not arguments or arguments == ["--check"]


def _report_legacy_aggregate_unavailable() -> int:
    print(_LEGACY_AGGREGATE_UNAVAILABLE_MESSAGE, file=sys.stderr)
    return 78


# Keep the historical command boundary ahead of argparse, pathlib, repository
# path resolution, and every application import.  The old no-argument and
# ``--check`` forms represented the removed aggregate generator and must not
# silently operate on the one retained output.
if __name__ == "__main__" and _is_legacy_aggregate_invocation(sys.argv[1:]):
    raise SystemExit(_report_legacy_aggregate_unavailable())


import argparse
import json
import re
import time
from pathlib import Path
from typing import Callable


REPO_ROOT = Path(__file__).resolve().parents[1]
REF_DIR = REPO_ROOT / "docs" / "reference"
REST_REFERENCE_PATH = REF_DIR / "REST_API.md"
HEADLESS_REST_REFERENCE_PATH = REF_DIR / "HEADLESS_REST_API.md"
MCP_REFERENCE_PATH = REF_DIR / "MCP_RUNTIME_CONTRACT.md"

# Composer is importable from the repository root, while manyforge_behavior
# uses a nested package root. Keep direct invocation deterministic instead of
# relying on an ambient PYTHONPATH from a particular container entrypoint.
for source_root in (REPO_ROOT, REPO_ROOT / "manyforge_behavior"):
    source_text = str(source_root)
    if source_text not in sys.path:
        sys.path.insert(0, source_text)


HEADER_TEMPLATE = (
    "<!--\n"
    "  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.\n"
    "  Regenerate with:\n"
    "    cd <repository-root>\n"
    "    python3 scripts/regen-public-contracts.py {selector}\n"
    "  Last regenerated: {timestamp}\n"
    "  Generator source: scripts/regen-public-contracts.py:{generator_name}\n"
    "-->\n\n"
)


def _stamp(generator_name: str, selector: str) -> str:
    return HEADER_TEMPLATE.format(
        timestamp=time.strftime("%Y-%m-%d %H:%M:%S %Z"),
        generator_name=generator_name,
        selector=selector,
    )


# ``--check`` compares generated content to disk. Normalize the timestamp on
# both sides so a fresh clock value is not reported as contract drift.
_TIMESTAMP_LINE_RE = re.compile(r"  Last regenerated: .*\n")


def _normalized_generated_content(content: str) -> str:
    return _TIMESTAMP_LINE_RE.sub(
        "  Last regenerated: <normalized>\n",
        content,
    )


def _markdown_cell(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ").strip()


def _append_openapi_routes(body: list[str], spec: dict) -> None:
    body.append(
        "The live schema is available at `/openapi.json` and `/docs` while "
        "the API process is running.\n\n"
    )
    body.append(
        f"OpenAPI version: `{spec.get('openapi', '?')}`. "
        f"Title: `{spec.get('info', {}).get('title', '?')}`.\n\n"
    )
    body.append("## Routes\n\n")
    body.append("| Method | Path | Tag | Summary |\n|---|---|---|---|\n")
    paths = spec.get("paths", {})
    for path, methods in sorted(paths.items()):
        for method, operation in sorted((methods or {}).items()):
            if method.upper() not in {"GET", "POST", "PUT", "DELETE", "PATCH"}:
                continue
            summary_lines = (
                operation.get("summary") or operation.get("description") or ""
            ).splitlines()
            summary = summary_lines[0] if summary_lines else ""
            tag = ", ".join(operation.get("tags") or [])
            body.append(
                f"| `{method.upper()}` | `{_markdown_cell(path)}` | "
                f"{_markdown_cell(tag)} | {_markdown_cell(summary)} |\n"
            )


def gen_rest_api() -> str:
    """Render the retained Composer REST surface from FastAPI OpenAPI."""
    from manyforge_composer.backend.app import create_app
    from manyforge_composer.backend.state import ComposerState

    # Route construction is transport-neutral. A schema-only sentinel exposes
    # the configured thin-client superset without making a request or starting
    # a Headless process, so the reference does not silently omit optional
    # runtime-client routes.
    spec = create_app(
        ComposerState(headless_client=object()),  # type: ignore[arg-type]
    ).openapi()
    body: list[str] = [
        _stamp("gen_rest_api", "structural-rest"),
        "# Composer REST API\n\n",
    ]
    body.append(
        "This is the structural, authoring, and thin runtime-client Composer "
        "surface retained during the composable-cell runtime cutover. Catalog "
        "reads, sandboxed artifact browsing, structural URDF assets, health, "
        "and the strict revision-fenced Program Draft/Saved workflow remain "
        "available. When Headless and the independent Program runner are "
        "configured, Composer also forwards scenario, Applied, Program Run, "
        "status, explicit Program Stop, and confirmed CCLC lifecycle/route "
        "requests without owning their state. Generic `/api/control/state` "
        "and `/api/control/commands` expose the same state-conditioned ingress "
        "used by the versioned `/ws/control` session; callers submit once and "
        "observe backend-owned lifecycle state. Complete product launch and "
        "remaining provider-specific capabilities remain unavailable. Build-owned "
        "skills are discovered and expanded into ordinary Program trees through "
        "`/api/skills`; expansion does not edit Draft or execute a command. The "
        "catch-all `/api/{unavailable_path}` operations return the typed "
        "runtime-unavailable response for former product routes.\n\n"
    )
    _append_openapi_routes(body, spec)
    return "".join(body)


def gen_headless_rest_api() -> str:
    """Render the independent H0 REST surface from its FastAPI owner."""
    from manyforge_headless_api.app import create_app

    # Route construction is service-neutral. The schema-only sentinel is never
    # started and no endpoint callback is invoked during OpenAPI generation.
    spec = create_app(service=object()).openapi()
    body: list[str] = [
        _stamp("gen_headless_rest_api", "headless-rest"),
        "# Headless Runtime REST API\n\n",
    ]
    body.append(
        "This is the independent headless projection over Manager, "
        "logical-scene, scene-runtime, Alarm Lifecycle, Historian, and the "
        "selected addressed execution owner and independent Program runner. "
        "Reads return owner-issued facts. Alarm acknowledgement, Manager and "
        "Program Apply, execution submission, no-motion preview, Program Run, "
        "explicit Program Stop, and confirmed CCLC lifecycle/route requests "
        "are attributed intents. Generic `/api/control/state` and "
        "`/api/control/commands` provide the freshness-aware REST form of the "
        "same transport-neutral control ingress used by `/ws/control`; no "
        "command is automatically replayed. The API never owns or reduces execution, "
        "Program-run, lifecycle, or controller-route state. Preview cannot dispatch, "
        "and approval uses the ordinary operation endpoint. An unconfigured or "
        "lost Program runner makes Program-owned routes unavailable and is never "
        "interpreted as idle; independently owned native operation and Stop "
        "routes remain available when their owner is configured. No condition "
        "mutation endpoint exists. `/api/health` reports only "
        "`headless_projection_ready`; robot readiness has no H0 owner and is "
        "explicitly unavailable. The alarm page defaults to 32 records and is "
        "bounded at 128, history defaults to and is bounded at 32 records, "
        "ordinary request bodies are bounded at 1 MiB, Program Apply at 17 MiB, "
        "model-attempt audit at 256 KiB, native/HTTP responses at 64 MiB, and "
        "private Program-runner responses at 32 MiB. Program diagnostic captures "
        "are separately limited to 64 KiB and asynchronously written by the existing "
        "Historian. Their producer-clock values, omissions and delivery losses are "
        "available through History/debug export and recorder health, not the "
        "high-rate control/viewer stream. Skill expansion is effect-free, selects "
        "one exact installed NodeEnvironment and requires ordinary Program/Binding "
        "validation before any subsequent execution.\n\n"
    )
    from manyforge_headless_api.models import GripperConfirmationRequest

    body.extend(
        [
            "## Explicit operator gripper confirmation\n\n",
            "Submit `gripper_confirm` once through `/api/control/commands` or the "
            "same control WebSocket. Read a current `/api/control/state` freshness "
            "context and the native execution owner epoch first. The payload uses "
            "the DTO below plus `expected_execution_owner_epoch`. The transport "
            "retains `request_context.request_id` but replaces its owner/source "
            "attribution from the admitted envelope. No caller timestamp, validity "
            "duration, sensor source or automatic retry is accepted.\n\n",
            "This requires an explicitly qualified retained "
            "`tool.gripper.confirmation.operator` binding and configured validity; "
            "maintained defaults do not enable it. It records an attributed "
            "operator statement for one completed command, not sensor proof or "
            "attachment permission. Query `gripper_confirmation` at the existing "
            "target-observation endpoint; expired evidence is trace only. Program "
            "`gripper.wait_for_status` consumes the same current evidence.\n\n",
            "The result carries native confirmation evidence and `history_recorded`. "
            "That flag means both bounded History submissions were queued, not "
            "synchronous disk durability. A History failure cannot change native "
            "acceptance; a lost reply remains unconfirmed and must not be resent "
            "as a new physical action.\n\n",
            "```json\n",
            json.dumps(GripperConfirmationRequest.model_json_schema(), indent=2, sort_keys=True),
            "\n```\n\n",
        ]
    )
    _append_openapi_routes(body, spec)
    return "".join(body)


def gen_mcp_api() -> str:
    """Render the optional adapter's real route-derived tool schemas."""
    from manyforge_composer.backend.app import create_app
    from manyforge_composer.backend.mcp_adapter import tool_contracts
    from manyforge_composer.backend.state import ComposerState

    app = create_app(ComposerState(headless_client=object()))
    contracts = tool_contracts(app)
    body = [
        _stamp("gen_mcp_api", "mcp"),
        "# ManyForge MCP HTTP contract\n\n",
        "Optional Streamable HTTP at `/mcp/`, enabled with Composer's `--mcp-http` "
        "and the `manyforge_composer[mcp]` extra. Disabled by default. "
        "The adapter is stateless: no MCP session ID, replay store, SSE fallback, "
        "heartbeat or background polling. It accepts the SDK initialize/tools/list/"
        "tools/call protocol; separate calls do not share execution authority.\n\n",
        "Each action delegates to the listed existing REST route. Supply its exact "
        "body and current owner-issued identities/confirmation/freshness; the adapter "
        "does not manufacture these, infer a target, retry or weaken admission. "
        "Read current backend state after commands. A lost tool response never proves "
        "no effect and must not trigger a new motion command. Kernel and Program "
        "Runner retain lifecycle ownership and reserved Stop capacity.\n\n",
        "Requests are limited to 1 MiB; owner response/query bounds also apply. "
        "Configure `MANYFORGE_MCP_ALLOWED_HOSTS` for remote Host names/IPs and "
        "`MANYFORGE_CORS_ORIGINS` for browser origins. Default MCP hosts are loopback. "
        "These are transport bounds, not caller authorization. Use the deployment's "
        "trusted network/perimeter; request attribution is not authentication.\n\n",
        "JSON route bodies are returned as text with `isError` for HTTP failures; "
        "viewer snapshots return MCP image content and require an existing matching "
        "viewer. Tools list only configured routes; capability availability is still "
        "determined by their owners. This is the current wire contract, not full "
        "historical-functional or provider qualification. Historical v0 signatures "
        "are preserved in git at `08222188293df8e12f90dc72e204c21f22f92ea1`.\n\n",
    ]
    for tool in contracts:
        body.extend(
            [
                f"## {tool['name']}\n\n",
                tool["description"] + "\n\n",
                "```json\n",
                json.dumps(tool, indent=2, sort_keys=True),
                "\n```\n\n",
            ]
        )
    return "".join(body).rstrip() + "\n"


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if _is_legacy_aggregate_invocation(arguments):
        return _report_legacy_aggregate_unavailable()

    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    subparsers = parser.add_subparsers(dest="selector", required=True)
    structural_rest = subparsers.add_parser(
        "structural-rest",
        help="Regenerate only the retained structural Composer REST reference.",
    )
    structural_rest.add_argument(
        "--check",
        action="store_true",
        help="Exit 1 if regeneration would change REST_API.md.",
    )
    headless_rest = subparsers.add_parser(
        "headless-rest",
        help="Regenerate only the independent I5/H0 REST reference.",
    )
    headless_rest.add_argument(
        "--check",
        action="store_true",
        help="Exit 1 if regeneration would change HEADLESS_REST_API.md.",
    )
    mcp = subparsers.add_parser("mcp", help="Regenerate the optional route-derived MCP contract.")
    mcp.add_argument("--check", action="store_true")
    args = parser.parse_args(arguments)

    targets: dict[Path, Callable[[], str]] = {
        "structural-rest": {REST_REFERENCE_PATH: gen_rest_api},
        "headless-rest": {HEADLESS_REST_REFERENCE_PATH: gen_headless_rest_api},
        "mcp": {MCP_REFERENCE_PATH: gen_mcp_api},
    }[args.selector]
    drift: list[str] = []
    for path, generator in targets.items():
        new_content = generator()
        old_content = path.read_text(encoding="utf-8") if path.exists() else ""
        if _normalized_generated_content(new_content) == (
            _normalized_generated_content(old_content)
        ):
            if not args.check:
                print(f"  unchanged {path.relative_to(REPO_ROOT)}")
            continue
        if args.check:
            drift.append(str(path.relative_to(REPO_ROOT)))
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(new_content, encoding="utf-8")
        print(f"  wrote {path.relative_to(REPO_ROOT)} ({len(new_content)} chars)")

    if drift:
        print("ERROR: regen would change: " + ", ".join(drift), file=sys.stderr)
        print(
            f"Run scripts/regen-public-contracts.py {args.selector} to update.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
