#!/usr/bin/env python3
"""Regenerate code-owned Composer/Headless REST or optional MCP references.

The former aggregate assistant, viewer, and lifecycle contract owners are
removed. The explicit generators are:

  - ``docs/reference/REST_API.md`` from
    ``create_app(ComposerState()).openapi()``.
  - ``docs/reference/HEADLESS_REST_API.md`` from the independent headless FastAPI
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


_UNAVAILABLE_REASON_CODE = "runtime_unavailable"
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
    # Owner-maintained request/outcome notes (``x-manyforge-reference-note``).
    notes = [
        (path, method, operation["x-manyforge-reference-note"])
        for path, methods in sorted(paths.items())
        for method, operation in sorted((methods or {}).items())
        if isinstance(operation, dict) and operation.get("x-manyforge-reference-note")
    ]
    if notes:
        body.append("\n## Route notes\n")
        for path, method, note in notes:
            body.append(f"\n### `{method.upper()} {path}`\n\n{note.strip()}\n")


# --- Change requests (OD-333 (2)) -------------------------------------------
#
# The "Change requests" section of REST_API.md. Which routes are fenced and
# which fence fields each one carries come from the request schemas; the Draft
# a route edits comes from its request model's base class; the example bodies
# are validated against those models; refusal codes are the error-catalog
# constants; command-row members come from the row table itself. The few
# facts code cannot state (where a value is read, default ports) are the small
# tables below. Each one is checked against the generated OpenAPI paths and
# request schemas, so a renamed field, route or code fails the generation
# instead of leaving stale text.

_COMPOSER_DEFAULT_PORT = 9000  # manyforge_composer/__main__.py --port default
_HEADLESS_DEFAULT_PORT = 9010  # manyforge_headless_api/cli.py serve --port default
_WORKBENCH_FRONTEND_PORT = 3000  # scripts/dev-workbench.py --frontend-port default
_WORKBENCH_COMPOSER_PORT = 9020  # scripts/dev-workbench.py --composer-port default
_HEADLESS_ADMISSION_PATH = "/api/cell/authoring-admission"
_RUNTIME_PATH = "/api/runtime"
_WORKSPACE_PATHS = ("/api/program/workspace", "/api/deployment/workspace")
_APPLY_ROWS_PATH = "/api/deployment/workspace"
_SUBMISSION_OUTCOME_PATH = "/api/authoring/submission-outcome"
_MANAGER_APPLY_STATUS_PATH = "/api/manager/apply/status"
# Member names written as literals in the route code (routes_draft._workspace,
# routes_deployment_draft._workspace, headless_ws._local_shell).
_WORKSPACE_MEMBER = "authoringWorkspace"
_APPLY_ROWS_MEMBER = "apply_commands"

# Fence fields as they appear on the wire: (name, meaning, read-from key,
# example value). The read-from key selects a text in _fence_sources().
_FENCE_FIELDS: tuple[tuple[str, str, str, object], ...] = (
    ("expectedAdmissionGeneration", "The cell's authoring admission counter.",
     "admission", 7),
    ("applicationStartId", "This start of ManyForge (16 lowercase hex). It "
     "changes when ManyForge restarts.", "start", "3f9a00000000ffff"),
    ("requestKey", "The request key of this attempt.", "key",
     "9c0e5d2a7b4f41e8a3d6c1b0f2e48a17"),
    ("expectedWorkspaceEpoch", "The authoring workspace instance.",
     "workspaceEpoch", "workspace-0d4c9e2b8f7a4c11b5e3a9d6f0c2b871"),
    ("expectedWorkspaceRevision", "The authoring workspace revision.",
     "workspaceRevision", 12),
    ("draftId", "The Draft this route edits (see the Draft column).", "draftId",
     None),
    ("expectedDraftRevision", "The revision of that Draft.", "draftRevision",
     None),
    ("deploymentDraftId", "The Deployment Draft.", "deploymentDraftId",
     "deployment-draft-5b7e1c9a2d4f4e60a8b3c7d1e9f0a246"),
    ("expectedDeploymentDraftRevision", "The Deployment Draft revision.",
     "deploymentDraftRevision", 3),
    ("request_key", "The request key of this attempt.", "key",
     "9c0e5d2a7b4f41e8a3d6c1b0f2e48a17"),
    ("application_start_id", "This start of ManyForge (16 lowercase hex).",
     "start", "3f9a00000000ffff"),
    ("expected_admission_generation", "The cell's authoring admission counter.",
     "admission", 7),
    ("expected_draft_id", "The Program Draft.", "programDraftId", None),
    ("expected_draft_revision", "The Program Draft revision.",
     "programDraftRevision", None),
    ("expected_authoring_workspace", "The whole authoring workspace identity.",
     "workspaceObject", None),
    ("request_context", "An object: `request_key` (the request key of this "
     "attempt) and `source_label` (free text naming the sender).", "keyObject", None),
)
_PROGRAM_EXAMPLE = {"draftId": "draft-1f6a3c8e9b2d4a7f8e0c5b3d2a1f9e46",
                    "expectedDraftRevision": 5}
_DEPLOYMENT_EXAMPLE = {"draftId": "deployment-draft-5b7e1c9a2d4f4e60a8b3c7d1e9f0a246",
                       "expectedDraftRevision": 3}
_SCENARIO_EXAMPLE_FIELDS = {"directory": "/workspace/scenarios", "name": "my_cell"}

# Apply commands: (method, path, row kind, when the row ends). Each kind is the
# literal its route passes to ApplyCommands.run().
_APPLY_COMMANDS: tuple[tuple[str, str, str, str], ...] = (
    ("POST", "/api/deployment/apply", "deployment_apply",
     "when the Apply has finished"),
    ("POST", "/api/deployment/revert", "deployment_revert",
     "when the Revert has finished"),
    ("POST", "/api/program/apply", "program_apply",
     "when the Manager has accepted the Apply"),
    ("POST", "/api/program/revert", "program_revert",
     "when the Manager has accepted the Apply"),
    ("POST", "/api/scenario/load", "scenario_load",
     "when the Load has finished"),
    ("POST", "/api/scenario/new", "scenario_new", "when the New has finished"),
)


def _fence_sources(admission_in_composer: bool, identity_keys: set[str]) -> dict[str, str]:
    def workspace(key: str) -> str:
        if key not in identity_keys:
            raise RuntimeError(f"authoring workspace identity lost {key!r}")
        return f"`{_WORKSPACE_MEMBER}.{key}` of `GET {_WORKSPACE_PATHS[0]}` or `GET {_WORKSPACE_PATHS[1]}`"

    admission = (
        f"`admission_generation` of `GET {_HEADLESS_ADMISSION_PATH}`"
        if admission_in_composer
        else f"`admission_generation` of `GET {_HEADLESS_ADMISSION_PATH}` on the "
        f"Headless API (port {_HEADLESS_DEFAULT_PORT}; Composer does not serve "
        "it), or `shell.authoring_admission.admission_generation` of a "
        "`/ws/control` snapshot"
    )
    return {
        "admission": admission,
        "start": f"`application_start_id` of `GET {_RUNTIME_PATH}`",
        "key": "Make a new one: 32 random hex characters",
        "keyObject": "Put a new request key in `request_key`",
        "workspaceEpoch": workspace("workspaceEpoch"),
        "workspaceRevision": workspace("workspaceRevision"),
        "draftId": (
            "Draft column Program: "
            + workspace("programDraftId").split(" of ")[0]
            + "; Deployment: "
            + workspace("deploymentDraftId").split(" of ")[0]
            + " (same reads as the workspace fields)"
        ),
        "draftRevision": (
            "Draft column Program: "
            + workspace("programDraftRevision").split(" of ")[0]
            + "; Deployment: "
            + workspace("deploymentDraftRevision").split(" of ")[0]
        ),
        "deploymentDraftId": workspace("deploymentDraftId"),
        "deploymentDraftRevision": workspace("deploymentDraftRevision"),
        "programDraftId": workspace("programDraftId"),
        "programDraftRevision": workspace("programDraftRevision"),
        "workspaceObject": f"The whole `{_WORKSPACE_MEMBER}` object, unchanged",
    }


def _resolve_schema(spec: dict, root: dict, schema: object) -> dict:
    for _ in range(16):
        if not isinstance(schema, dict):
            return {}
        ref = schema.get("$ref")
        if not isinstance(ref, str):
            return schema
        name = ref.rsplit("/", 1)[-1]
        if ref.startswith("#/components/schemas/"):
            schema = spec.get("components", {}).get("schemas", {}).get(name)
        elif ref.startswith("#/$defs/"):
            schema = root.get("$defs", {}).get(name)
        else:
            return {}
    return {}


def _body_fields(spec: dict, operation: dict) -> list[str]:
    """Top-level JSON body member names of one OpenAPI operation, in order."""

    content = (operation.get("requestBody") or {}).get("content") or {}
    root = (content.get("application/json") or {}).get("schema")
    if not isinstance(root, dict):
        return []
    names: list[str] = []
    pending = [root]
    while pending:
        schema = _resolve_schema(spec, root, pending.pop(0))
        names.extend(name for name in schema.get("properties") or {} if name not in names)
        pending.extend(schema.get("allOf") or ())
    return names


def _operations(spec: dict):
    for path, methods in sorted(spec.get("paths", {}).items()):
        for method, operation in sorted((methods or {}).items()):
            if isinstance(operation, dict) and method.upper() in {"POST", "PUT", "PATCH", "DELETE"}:
                yield method.upper(), path, operation


def _json_block(value: object) -> str:
    return "```json\n" + json.dumps(value, indent=2) + "\n```\n\n"


def _append_change_requests(body: list[str], spec: dict, facts: dict) -> None:
    """Render the Change requests section from code-derived ``facts``."""

    sources = facts["sources"]
    fence_names = [name for name, *_ in _FENCE_FIELDS]
    fenced: list[tuple[str, str, str, list[str]]] = []
    seen: set[str] = set()
    for method, path, operation in _operations(spec):
        fields = [name for name in _body_fields(spec, operation) if name in fence_names]
        # `draftId` fences only beside the revision it names (the Draft fences
        # hand both to the commit); alone it is a recorded identity, e.g. the
        # Reset-then-Apply log context, and checks nothing.
        if "draftId" in fields and "expectedDraftRevision" not in fields:
            fields.remove("draftId")
        if not fields:
            continue
        seen.update(fields)
        draft = facts["drafts"].get((method, path), "—")
        if draft == "—" and {"draftId", "expectedDraftRevision"} & set(fields):
            raise RuntimeError(f"{method} {path}: draftId without a known Draft base model")
        fields.sort(key=fence_names.index)
        fenced.append((method, path, draft, fields))
    missing = [name for name in fence_names if name not in seen]
    if missing:
        raise RuntimeError("fence fields no longer in any request body: " + ", ".join(missing))
    for path in (_RUNTIME_PATH, *_WORKSPACE_PATHS, _SUBMISSION_OUTCOME_PATH,
                 _MANAGER_APPLY_STATUS_PATH):
        if "get" not in spec.get("paths", {}).get(path, {}):
            raise RuntimeError(f"GET {path} is no longer a Composer route")
    for method, path, _kind, _ends in _APPLY_COMMANDS:
        operation = spec.get("paths", {}).get(path, {}).get(method.lower())
        if not isinstance(operation, dict) or not {"request_key", "requestKey"} & set(
            _body_fields(spec, operation)
        ):
            raise RuntimeError(f"{method} {path} is no longer an Apply command with a request key")

    codes = facts["codes"]
    row = facts["row_members"]
    end = facts["end_members"]
    body.append("\n## Change requests\n\n")
    body.append(
        "This section says how to send a request that changes something, so "
        "that it is accepted, and what to do when it is refused. It is "
        "generated from the request models.\n\n"
    )
    body.append("### Where to send\n\n")
    body.append(
        f"Composer serves every path on this page at `http://<host>:{_COMPOSER_DEFAULT_PORT}` "
        f"(the default `--port` of `python -m manyforge_composer`). "
        f"On the development workbench, Composer listens on port "
        f"{_WORKBENCH_COMPOSER_PORT}; while the workbench runs the development GUI, "
        f"the GUI address on port {_WORKBENCH_FRONTEND_PORT} forwards `/api` too. "
        "Bodies are JSON (`Content-Type: application/json`). Some routes use "
        "camelCase names (`requestKey`), others snake_case names "
        "(`request_key`). Send each name exactly as this page shows it for "
        "that route; do not convert between the two styles. A body with an "
        f"unknown or missing field is refused with `{codes['invalid']}` (HTTP 422).\n\n"
    )
    body.append("### Terms\n\n")
    body.append("| Term | Meaning |\n|---|---|\n")
    for term, meaning in (
        ("change request", "A request that changes a Draft, the authoring workspace or the cell."),
        ("fence field", "A body field that repeats a value the client read. The server "
         "compares it with the current value. If they differ, the server refuses "
         "the request and changes nothing."),
        ("request key", "32 random hex characters made by the client. One request key "
         "is one attempt. A new attempt gets a new request key."),
        ("refusal", "An error answer with a typed `detail.code`."),
        ("Apply command", "Apply, Revert, Load or New. The server answers as soon as it "
         "accepts the command. The command then runs; the client reads its row to see "
         "when it ends and how."),
        ("row", "The server's record of one Apply command: its state while it runs and its result when it has ended."),
    ):
        body.append(f"| {term} | {meaning} |\n")
    body.append("\n### How to send a change request\n\n")
    for number, step in enumerate((
        "Read the current values. The table *Fence fields* says where each value is "
        "read. Read them just before you send.",
        "Make a new request key: 32 random hex characters (for example Python "
        "`secrets.token_hex(16)`).",
        "Build the body: the route's own fields (see `/openapi.json`) plus every "
        "fence field the table *Fenced requests* lists for the route.",
        "Send the request once.",
        "Read the answer. A 2xx answer means it was done. For an Apply command, "
        "2xx means only *accepted*: read its row until the row has ended (see "
        "*Apply commands*).",
        "If the answer is a refusal, look up `detail.code` in *Refusals* and do "
        "what it says. To try again, start at step 1: read the values again, make "
        "a new request key and build a new body. Never send an old body again "
        "after a refusal.",
        "If you got no answer at all (timeout, lost connection), you do not know "
        "whether it was done. Do not make a new attempt yet. Check first (see "
        "*No answer*).",
    ), start=1):
        body.append(f"{number}. {step}\n")
    body.append("\n### Fence fields\n\n")
    body.append("| Field | Meaning | Read from |\n|---|---|---|\n")
    for name, meaning, source, _example in _FENCE_FIELDS:
        body.append(f"| `{name}` | {_markdown_cell(meaning)} | {_markdown_cell(sources[source])} |\n")
    body.append(
        "\nThe Draft column of the next table says which Draft `draftId` and "
        "`expectedDraftRevision` name. *Program and Deployment*: `draftId` is "
        "the Program Draft and `deploymentDraftId` the Deployment Draft.\n\n"
    )
    body.append("### Fenced requests\n\n")
    body.append("| Method | Path | Draft | Fence fields |\n|---|---|---|---|\n")
    for method, path, draft, fields in fenced:
        listed = ", ".join(f"`{name}`" for name in fields)
        body.append(f"| `{method}` | `{_markdown_cell(path)}` | {draft} | {listed} |\n")
    body.append("\n### Example bodies\n\n")
    for title, path, example, origin in facts["examples"]:
        body.append(f"**{title}** — `POST {path}`:\n\n")
        body.append(_json_block(example))
        body.append("Where each value came from:\n\n")
        for name in example:
            body.append(f"- `{name}`: {origin.get(name, 'this route’s own field (see `/openapi.json`)')}\n")
        body.append("\n")
    body.append("### Refusals\n\n")
    body.append(
        "A refusal has the HTTP status of the error and this body: "
        "`{\"detail\": {\"code\": ..., \"message\": ..., \"producer\": ...}}`. "
        "Decide by `detail.code`, never by `message`. When `detail.effect` is "
        "`\"none\"`, the request changed nothing. Without that member, the "
        "request may have changed something: read the state before you try "
        "again.\n\n"
    )
    body.append("| `detail.code` | Meaning | What the client does |\n|---|---|---|\n")
    for code, meaning, action in facts["refusals"]:
        body.append(f"| `{code}` | {meaning} | {action} |\n")
    body.append("\n### No answer\n\n")
    body.append(
        f"- Apply command: send the same body again with the same request key. "
        "The server does not run it twice: it answers with the row of that "
        f"request key. Or read the row (see *Apply commands*).\n"
        f"- Other change request on a camelCase route: read `GET {_SUBMISSION_OUTCOME_PATH}"
        "?application_start_id=...&request_key=...&expected_admission_generation=...` "
        "with the values you sent. `outcome` not null: the request was done. "
        "`outcome` null: unknown. Read the workspace and check whether your "
        "change is there before you make a new attempt.\n"
        "- Any other request: read the state the request changes before you "
        "make a new attempt.\n\n"
    )
    body.append("### Apply commands\n\n")
    body.append(
        "These requests answer as soon as the command is accepted. The answer "
        f"is `{{\"schema_version\": \"{facts['reply_schema']}\", \"command\": <row>}}`.\n\n"
    )
    body.append("| Method | Path | Row `kind` | The row ends |\n|---|---|---|---|\n")
    for method, path, kind, ends in _APPLY_COMMANDS:
        body.append(f"| `{method}` | `{path}` | `{kind}` | {ends} |\n")
    body.append(
        f"\nRead the rows in `{_APPLY_ROWS_MEMBER}` of `GET {_APPLY_ROWS_PATH}` "
        f"(also in the `/ws/control` snapshot `shell`). `{_APPLY_ROWS_MEMBER}` "
        f"has `{facts['table_members'][0]}` (the command that runs now, or null) "
        f"and `{facts['table_members'][1]}` (the last {facts['ended_rows']} "
        "ended rows, newest first). Find your row by its `request_key`.\n\n"
        "A row has " + ", ".join(f"`{name}`" for name in row) + ". "
        "`end` is null while the command runs. When the command has ended, "
        "`end` has " + ", ".join(f"`{name}`" for name in end) + ". "
        "`end.outcome` is one of " + ", ".join(f"`{name}`" for name in facts["outcomes"])
        + "; `end.code` is null exactly when the outcome is "
        f"`{facts['outcomes'][0]}`.\n\n"
        "An Apply command's row also has `step_source`: what the command "
        "waits for now, or null. When `step_source.status` is `manager_apply`, "
        "it names the Manager Apply (`apply_id`, `apply_generation`). Read that "
        f"Apply's details at `GET {_MANAGER_APPLY_STATUS_PATH}?apply_id=...&apply_generation=...`.\n\n"
        "Only one Apply command runs at a time.\n"
    )


def _change_request_facts(app: object, spec: dict) -> dict:
    """Code-owned facts of the Change requests section."""

    from typing import get_args

    from fastapi.routing import APIRoute
    from manyforge_headless_api import error_codes as codes
    from manyforge_headless_api.command_rows import CommandRowTable
    from manyforge_headless_api.program_cycle import (
        MAXIMUM_ENDED_COMMAND_ROWS,
        CommandOutcome,
    )
    from manyforge_headless_api.request_validation import REQUEST_INVALID_CODE
    from manyforge_composer.backend import (
        routes_deployment_draft,
        routes_draft,
        routes_scenario_documents,
    )
    from manyforge_composer.backend.apply_orchestration import (
        APPLY_COMMAND_REPLY_SCHEMA,
        ApplyCommands,
    )
    from manyforge_composer.backend.authoring_workspace import AuthoringWorkspaceIdentity
    from manyforge_composer.backend.routes_assistant_draft import (
        AssistantDraftCallbackRequest,
    )
    from manyforge_composer.backend.routes_headless import (
        ComposerSceneTransactionRequest,
    )

    domains = (
        (routes_scenario_documents._ScenarioFence, "Program and Deployment"),
        (routes_draft._DraftFence, "Program"),
        (routes_deployment_draft._Fence, "Deployment"),
        (AssistantDraftCallbackRequest, "Program"),
        (ComposerSceneTransactionRequest, "Program"),
    )
    drafts: dict[tuple[str, str], str] = {}
    models: dict[type, str] = {}
    for route in app.routes:  # type: ignore[attr-defined]
        field = getattr(route, "body_field", None) if isinstance(route, APIRoute) else None
        model = None
        if field is not None:
            model = getattr(field, "type_", None) or field.field_info.annotation
        if not isinstance(model, type):
            continue
        for method in route.methods:
            models.setdefault(model, route.path)
            for base, name in domains:
                if issubclass(model, base):
                    drafts[(method.upper(), route.path)] = name
                    break

    identity = AuthoringWorkspaceIdentity(
        workspace_epoch="workspace-0d4c9e2b8f7a4c11b5e3a9d6f0c2b871",
        workspace_revision=12,
        program_draft_id=_PROGRAM_EXAMPLE["draftId"],
        program_draft_revision=_PROGRAM_EXAMPLE["expectedDraftRevision"],
        deployment_draft_id=_DEPLOYMENT_EXAMPLE["draftId"],
        deployment_draft_revision=_DEPLOYMENT_EXAMPLE["expectedDraftRevision"],
    ).as_dict()
    admission_in_composer = "get" in spec.get("paths", {}).get(_HEADLESS_ADMISSION_PATH, {})
    sources = _fence_sources(admission_in_composer, set(identity))
    example_values = {name: value for name, _m, _s, value in _FENCE_FIELDS}
    field_source = {name: source for name, _m, source, _v in _FENCE_FIELDS}

    def example(model: type, title: str, draft: dict[str, object] | None,
                extra: dict[str, object]) -> tuple[str, str, dict, dict]:
        path = models.get(model)
        if path is None:
            raise RuntimeError(f"{model.__name__} is no longer a route body")
        aliases = [info.alias or name for name, info in model.model_fields.items()]
        value: dict[str, object] = {}
        origin: dict[str, str] = {}
        for alias in aliases:
            if alias in extra:
                value[alias] = extra[alias]
            elif draft is not None and alias in draft:
                value[alias] = draft[alias]
            elif alias in example_values and example_values[alias] is not None:
                value[alias] = example_values[alias]
            else:
                continue
            if alias in field_source:
                origin[alias] = sources[field_source[alias]]
        model.model_validate(value)  # the example is a valid body
        return title, path, value, origin

    examples = [
        example(routes_draft.DiscardDraftRequest, "Program Draft (Discard the Program)",
                _PROGRAM_EXAMPLE, {}),
        example(routes_deployment_draft.DiscardDeploymentRequest,
                "Deployment Draft (Discard the Deployment)", _DEPLOYMENT_EXAMPLE, {}),
        example(routes_scenario_documents.SaveScenarioDocumentRequest,
                "Scenario document (Save the Scenario)", _PROGRAM_EXAMPLE,
                _SCENARIO_EXAMPLE_FIELDS),
        example(routes_scenario_documents.ScenarioEmptyRequest,
                "Apply command (New)", None, {"discard_unsaved": False}),
    ]
    # Program and Deployment examples name each Draft in its own words.
    for index, draft_source in ((0, "programDraft"), (1, "deploymentDraft")):
        origin = examples[index][3]
        origin["draftId"] = sources[f"{draft_source}Id"]
        origin["expectedDraftRevision"] = sources[f"{draft_source}Revision"]
    examples[2][3]["draftId"] = sources["programDraftId"]
    examples[2][3]["expectedDraftRevision"] = sources["programDraftRevision"]
    examples[3][3]["discard_unsaved"] = (
        "`true` discards an unsaved Draft; with `false` New is refused while a Draft is unsaved"
    )

    reread = "Read the values again, make a new request key, send a new body."
    refusals = (
        (codes.COMPOSER_WORKSPACE_STALE, "The authoring workspace changed since you read it. "
         f"`detail.workspace` holds the current `{_WORKSPACE_MEMBER}`.", reread),
        (codes.COMPOSER_DRAFT_ID_STALE, "The Program Draft was replaced (Open, New, Load).", reread),
        (codes.COMPOSER_DRAFT_REVISION_STALE, "The Program Draft changed since you read it.", reread),
        (codes.DEPLOYMENT_DRAFT_STALE, "The Deployment Draft changed since you read it.", reread),
        (codes.SCENE_TRANSACTION_DRAFT_STALE, "The Program Draft changed since you read it.", reread),
        (codes.STALE_GENERATION, "The authoring admission counter moved.", reread),
        (codes.STALE_APPLICATION_START_ID, "ManyForge restarted. Every value read before "
         "the restart is old.", "Read all values again, from `GET /api/runtime` on; "
         "make a new request key, send a new body."),
        (codes.START_IDENTITY_REQUIRED, "The application start id is missing.",
         "Add it and send again with a new request key."),
        (codes.REQUEST_KEY_REQUIRED, "The request key is missing.",
         "Add a new request key and send again."),
        (codes.REQUEST_KEY_INVALID, "The request key is not 32 hex characters.",
         "Make a new request key and send again."),
        (REQUEST_INVALID_CODE, "The body does not match the route's schema (a missing "
         "or misspelled field, the other name style, a wrong type). `detail.details` "
         "names each bad field.", "Fix the body as `detail.details` says; use a new request key."),
        (codes.AUTHORING_NOT_READY, "The cell does not allow this change now (a Program "
         "run, a manual operation or an unfinished Apply).", "Do not resend at once. "
         "Wait until the cell is idle, then start again at step 1."),
        (codes.APPLY_IN_PROGRESS, "Another Apply command is running.",
         f"Wait until `{_APPLY_ROWS_MEMBER}.in_progress` is null, then start again at step 1."),
    )

    table = CommandRowTable(clock_ns=lambda: 1)
    key = "0" * 32
    accepted = table.accept("kind", key)
    ended = table.end(key, "failed", "code")
    return {
        "sources": sources,
        "drafts": drafts,
        "examples": examples,
        "refusals": refusals,
        "codes": {"invalid": REQUEST_INVALID_CODE},
        "reply_schema": APPLY_COMMAND_REPLY_SCHEMA,
        "table_members": tuple(ApplyCommands().snapshot()),
        "ended_rows": MAXIMUM_ENDED_COMMAND_ROWS,
        "row_members": tuple(accepted),
        "end_members": tuple(ended["end"]),
        "outcomes": get_args(CommandOutcome),
    }


def gen_rest_api() -> str:
    """Render the retained Composer REST surface from FastAPI OpenAPI."""
    from manyforge_composer.backend.app import create_app
    from manyforge_composer.backend.state import ComposerState

    # Route construction is transport-neutral. A schema-only sentinel exposes
    # the configured thin-client superset without making a request or starting
    # a Headless process, so the reference does not silently omit optional
    # runtime-client routes.
    app = create_app(
        ComposerState(headless_client=object(), assistant_client=object(), application_start_id=1),  # type: ignore[arg-type]
    )
    spec = app.openapi()
    body: list[str] = [
        _stamp("gen_rest_api", "structural-rest"),
        "# Composer REST API\n\n",
    ]
    body.append(
        "This is the structural, authoring, and thin runtime-client Composer "
        "surface. Catalog "
        "reads, sandboxed artifact browsing, structural URDF assets, health, "
        "and the strict revision-fenced Program Draft/Saved workflow remain "
        "available. When Headless and the independent Program runner are "
        "configured, Composer also forwards scenario, Applied, Program Run, "
        "status, Stops, component actions and the server-run "
        "Apply/Load/New requests without owning their state. Generic `/api/control/state` "
        "and `/api/control/commands` expose the same state-conditioned ingress "
        "used by the versioned `/ws/control` session; callers submit once and "
        "observe backend-owned lifecycle state. Complete product launch and "
        "remaining provider-specific capabilities remain unavailable. Build-owned "
        "skills are discovered and expanded into ordinary Program trees through "
        "`/api/skills`; expansion does not edit Draft or execute a command. The "
        "catch-all `/api/{unavailable_path}` operations return the typed "
        "runtime-unavailable response for former product routes.\n\n"
    )
    body.append(
        "**Real robot.** A real robot's address is a site fact "
        "(`device_addresses` in the `manyforge.site.v2` site settings file); "
        "a Scenario names only its key (`configuration.device_address_ref` of "
        "the machine owner). Component states, faults, the cell latch and "
        "readiness are readable here; the site file, robot power, network and "
        "e-stop are the operator's. See "
        "[Real robot setup](../operations/REAL_ROBOT_SETUP.md).\n\n"
    )
    _append_openapi_routes(body, spec)
    _append_change_requests(body, spec, _change_request_facts(app, spec))
    return "".join(body)


def gen_headless_rest_api() -> str:
    """Render the independent headless REST surface from its FastAPI owner."""
    from types import SimpleNamespace

    from manyforge_headless_api.app import create_app
    from manyforge_headless_api.identity import RequestAdmission

    # Route construction is service-neutral. The schema-only sentinel is never
    # started and no endpoint callback is invoked during OpenAPI generation;
    # it carries only the ``admission`` (``A`` check) that route construction reads.
    spec = create_app(
        service=SimpleNamespace(admission=RequestAdmission("0000000000000001")),  # type: ignore[arg-type]
    ).openapi()
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
        "Stops, and component actions are attributed intents. Generic `/api/control/state` and "
        "`/api/control/commands` provide the freshness-aware REST form of the "
        "same transport-neutral control ingress used by `/ws/control`; no "
        "command is automatically replayed. The API never owns or reduces execution, "
        "Program-run, lifecycle, or controller-route state. Preview cannot dispatch, "
        "and approval uses the ordinary operation endpoint. An unconfigured or "
        "lost Program runner makes Program-owned routes unavailable and is never "
        "interpreted as idle; independently owned native operation and Stop "
        "routes remain available when their owner is configured. No condition "
        "mutation endpoint exists. `/api/health` reports only "
        "`headless_projection_ready`; robot readiness has no headless owner and is "
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
    body.append(
        "The `request_key` of a Scene request (`/api/scene/transactions`) is "
        "recognised, and the request sent again is answered with its first "
        "result, until the Scene store folds its history into a snapshot. A "
        "request sent again after that is treated as a new request, with one "
        "exception: an authoring edit (or a Program node's Scene edit) whose "
        "admission is still the newest one settled for its owner is refused "
        "with `request_result_expired` and changes nothing.\n\n"
        "A Scene request that finds the Scene changed by another request since "
        "it was checked, or another Scene commit still in progress, is refused "
        "with `stale_revision` and has no effect; refresh the Scene and send it "
        "again as a new request (a new `request_key`).\n\n"
    )
    body.append(
        "A successful Scene commit is answered once Headless's own view of the "
        "logical scene shows the committed revision (or a newer one, or another "
        "active bundle, graph or scene), so a read after the reply shows the "
        "commit; when the Manager stops answering, or Headless shuts down, the "
        "success reply is returned at once. This wait has no time limit.\n\n"
    )
    body.append(
        "The Program runner reads and edits the scene, and latches pose "
        "observations into it between nodes, at the Manager, not through this "
        "API; a Program node's read after such a change shows it. Headless's "
        "own view of the logical scene shows these changes once the Manager "
        "publishes them, so a read through this API is not ordered with "
        "them.\n\n"
    )
    _append_openapi_routes(body, spec)
    return "".join(body)


def gen_mcp_api() -> str:
    """Render the optional adapter's real route-derived tool schemas."""
    from manyforge_composer.backend.app import create_app
    from manyforge_composer.backend.mcp_adapter import tool_contracts
    from manyforge_composer.backend.state import ComposerState

    app = create_app(ComposerState(headless_client=object(), assistant_client=object(), application_start_id=1))
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
        "The MCP library would turn NaN, Infinity or a number too large to hold "
        "(such as `1e999`) into an empty value, so a tool call holding a repeated "
        "member or such a number is refused as an invalid request (HTTP 400, "
        "JSON-RPC `-32600`) and nothing is carried out. A Stop holding such a "
        "number is not refused: a `manyforge_kernel` `stop` or `stop_all` whose "
        "body holds one is sent on with an empty body and becomes Stop all; a "
        "Stop command sent with `manyforge_cycle_control` whose payload holds one "
        "is sent on with an empty payload and becomes Stop all (a Halt stays a "
        "Halt).\n\n",
        "**Real robot.** `manyforge_components` reads component states (the "
        "machine's controller mode, state, error and motors in `details`), "
        "faults, the cell latch and readiness, and runs component actions. A "
        "failed Apply or Load names its cause (e.g. `site_settings_unavailable`). "
        "Ask the operator for the site settings file, a workbench restart, robot "
        "power and network, the e-stop and an attended session. See "
        "[Real robot setup](../operations/REAL_ROBOT_SETUP.md).\n\n",
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
        help="Regenerate only the independent headless REST reference.",
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
