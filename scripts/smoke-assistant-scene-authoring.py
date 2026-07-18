#!/usr/bin/env python3
"""Smoke the assistant scene-authoring path.

The script exercises the deterministic assistant path:

  inspect_isaac_scene -> propose_scene_objects

By default it calls the tool functions directly, so live ROS smoke checks can
run in the ROS-capable image without FastAPI installed.  Use --transport api to
exercise the Composer assistant endpoints in-process.  Use
--workflow proposal-lifecycle with --transport api to also validate
proposal staging and session-patch materialization without mutating the loaded
program.  With --fixture it uses a committed Isaac report fixture, which is the
mode used by deterministic tests.
"""

from __future__ import annotations

import argparse
import copy
import json
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from typing import Any


def main() -> int:
    args = _parse_args()
    repo_root = Path(__file__).resolve().parents[1]
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))
    for package_root in ("manyforge_behavior", "manyforge_composer", "manyforge_ros"):
        package_path = repo_root / package_root
        if package_path.is_dir() and str(package_path) not in sys.path:
            sys.path.insert(0, str(package_path))

    try:
        result = _run_smoke(args, repo_root)
    except Exception as exc:
        print(json.dumps({"success": False, "error": str(exc)}, sort_keys=True))
        return 2

    print(json.dumps(result, indent=2 if args.pretty else None, sort_keys=True))
    if not args.allow_empty and not result["propose"]["sceneResources"]:
        return 3
    return 0


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--transport",
        choices=["direct", "api"],
        default="direct",
        help="direct calls assistant tool functions; api calls Composer endpoints in-process.",
    )
    parser.add_argument(
        "--workflow",
        choices=["tools", "proposal-lifecycle"],
        default="tools",
        help=(
            "tools validates inspect->propose; proposal-lifecycle additionally "
            "stages and materializes a scene-resource proposal through Composer."
        ),
    )
    parser.add_argument(
        "--fixture",
        default=None,
        help="Use an offline Isaac scene report fixture instead of live ROS inspection.",
    )
    parser.add_argument(
        "--include-subtree",
        action="append",
        default=[],
        help="Isaac prim subtree to include during inspection; may be repeated.",
    )
    parser.add_argument(
        "--exclude-subtree",
        action="append",
        default=[],
        help="Isaac prim subtree to exclude during inspection; may be repeated.",
    )
    parser.add_argument(
        "--probe-prim",
        action="append",
        default=[],
        help="Non-collider prim to inspect for transform/bounds; may be repeated.",
    )
    parser.add_argument(
        "--source-root",
        action="append",
        default=[],
        help="Inspected source root to convert to a scene-resource proposal.",
    )
    parser.add_argument("--object-id-prefix", default="")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--max-colliders", type=int, default=None)
    parser.add_argument("--max-proposals", type=int, default=None)
    parser.add_argument("--include-hidden", action="store_true")
    parser.add_argument("--include-cooked-samples", action="store_true")
    parser.add_argument(
        "--no-cooked-convex",
        action="store_false",
        dest="include_cooked_convex",
    )
    parser.set_defaults(include_cooked_convex=True)
    parser.add_argument(
        "--output-profile",
        default="llm_scene_authoring",
        choices=["llm_scene_authoring", "raw_inventory"],
    )
    parser.add_argument(
        "--catalog-path",
        default=None,
        help="Composer catalog path. Defaults to the repo node catalog.",
    )
    parser.add_argument("--allow-empty", action="store_true")
    parser.add_argument("--pretty", action="store_true")
    return parser.parse_args()


def _run_smoke(args: argparse.Namespace, repo_root: Path) -> dict[str, Any]:
    if args.workflow == "proposal-lifecycle" and args.transport != "api":
        raise RuntimeError("--workflow proposal-lifecycle requires --transport api")
    if args.transport == "direct":
        return _run_direct_smoke(args)
    return _run_api_smoke(args, repo_root)


def _run_direct_smoke(args: argparse.Namespace) -> dict[str, Any]:
    from manyforge_composer.backend.assistant_tools import (
        inspect_isaac_scene_report,
        propose_scene_object_resources,
    )

    fixture_report = _load_fixture(args.fixture) if args.fixture else None
    request = _inspection_request(args)
    report = (
        _filter_report_for_request(fixture_report, request)
        if fixture_report is not None
        else inspect_isaac_scene_report(request)
    )
    propose_data = propose_scene_object_resources(
        report,
        source_roots=args.source_root,
        object_id_prefix=args.object_id_prefix,
        max_proposals=args.max_proposals,
    )
    return _smoke_result(
        mode="fixture" if fixture_report is not None else "live",
        transport="direct",
        report=report,
        propose_data=propose_data,
    )


def _run_api_smoke(args: argparse.Namespace, repo_root: Path) -> dict[str, Any]:
    from fastapi.testclient import TestClient

    from manyforge_behavior.kernel_client import MockKernelClient
    from manyforge_behavior.node_environment import build_node_environment
    from manyforge_composer.backend import routes_assistant
    from manyforge_composer.backend.app import create_app
    from manyforge_composer.backend.security import SecurityConfig
    from manyforge_composer.backend.state import ComposerState

    fixture_report = _load_fixture(args.fixture) if args.fixture else None
    original_inspector = routes_assistant.inspect_isaac_scene_report
    if fixture_report is not None:

        def fixture_inspector(request: Any) -> dict[str, Any]:
            return _filter_report_for_request(fixture_report, request)

        routes_assistant.inspect_isaac_scene_report = fixture_inspector

    catalog_path = args.catalog_path or str(
        repo_root / "manyforge_behavior" / "resources" / "node_catalog.yaml"
    )
    state = ComposerState(
        node_environment=build_node_environment(catalog_path),
        kernel=MockKernelClient(),
    )
    app = create_app(
        state,
        security=SecurityConfig(
            cors_origins=["http://localhost:3000"],
            api_token=None,
            allowed_fs_roots=[str(repo_root), "/workspace", "/tmp"],
        ),
    )

    try:
        with TestClient(app) as client:
            inspect_payload: dict[str, Any] = {
                "includeSubtrees": args.include_subtree,
                "excludeSubtrees": args.exclude_subtree,
                "probePrims": args.probe_prim,
                "includeHidden": args.include_hidden,
                "includeCookedConvex": args.include_cooked_convex,
                "includeCookedSamples": args.include_cooked_samples,
                "outputProfile": args.output_profile,
                "timeoutS": args.timeout,
            }
            if args.max_colliders is not None:
                inspect_payload["maxColliders"] = args.max_colliders

            inspect_response = client.post(
                "/api/assistant/tools/inspect-isaac-scene",
                json=inspect_payload,
            )
            inspect_data = _checked_response(inspect_response, "inspect_isaac_scene")
            report = inspect_data["report"]

            propose_payload: dict[str, Any] = {
                "inspectionReport": report,
                "sourceRoots": args.source_root,
                "objectIdPrefix": args.object_id_prefix,
            }
            if args.max_proposals is not None:
                propose_payload["maxProposals"] = args.max_proposals

            propose_response = client.post(
                "/api/assistant/tools/propose-scene-objects",
                json=propose_payload,
            )
            propose_data = _checked_response(propose_response, "propose_scene_objects")
            lifecycle_data = (
                _run_proposal_lifecycle(client, state, propose_data)
                if args.workflow == "proposal-lifecycle"
                else None
            )

        return _smoke_result(
            workflow=args.workflow,
            mode="fixture" if fixture_report is not None else "live",
            transport="api",
            report=report,
            propose_data=propose_data,
            inspect_mutated=inspect_data.get("mutated"),
            lifecycle_data=lifecycle_data,
        )
    finally:
        routes_assistant.inspect_isaac_scene_report = original_inspector


def _run_proposal_lifecycle(
    client: Any,
    state: Any,
    propose_data: dict[str, Any],
) -> dict[str, Any]:
    proposals = list(propose_data.get("proposals") or [])
    scene_resource_proposals = [
        proposal
        for proposal in proposals
        if isinstance(proposal, dict) and proposal.get("type") == "scene_resource_add"
    ]
    if not scene_resource_proposals:
        raise RuntimeError("proposal-lifecycle requires at least one scene_resource_add proposal")

    with tempfile.TemporaryDirectory(prefix="manyforge-assistant-smoke-") as tmp_dir:
        program_path = _write_minimal_program(Path(tmp_dir))
        state.load_program(str(program_path))

        before_generation = state.program_session_generation
        before_scene_count = len(state.program.scene) if state.program else None
        proposal = scene_resource_proposals[0]

        stage_response = client.post(
            "/api/assistant/proposals/stage",
            json={
                "conversationId": "smoke-assistant-scene-authoring",
                "proposals": [proposal],
                "source": {
                    "kind": "smoke",
                    "workflow": "proposal-lifecycle",
                    "transport": "api",
                },
            },
        )
        stage_data = _checked_response(stage_response, "stage_assistant_proposals")
        staged = stage_data.get("proposals") or []
        if len(staged) != 1:
            raise RuntimeError("proposal staging did not return exactly one proposal")

        proposal_id = staged[0]["proposalId"]
        materialize_response = client.post(f"/api/assistant/proposals/{proposal_id}/materialize")
        materialize_data = _checked_response(materialize_response, "materialize_assistant_proposal")

        after_scene_count = len(state.program.scene) if state.program else None
        if state.program_session_generation != before_generation:
            raise RuntimeError("proposal lifecycle smoke mutated program session generation")
        if after_scene_count != before_scene_count:
            raise RuntimeError("proposal lifecycle smoke mutated loaded program scene")

    return {
        "stage": {
            "proposalId": proposal_id,
            "proposalCount": len(staged),
            "mutated": stage_data.get("mutated"),
            "programMutated": stage_data.get("programMutated"),
        },
        "materialize": {
            "proposalId": materialize_data.get("proposalId"),
            "sessionPatch": materialize_data.get("sessionPatch"),
            "mutated": materialize_data.get("mutated"),
            "programMutated": materialize_data.get("programMutated"),
        },
        "loadedProgram": {
            "sceneCountBefore": before_scene_count,
            "sceneCountAfter": after_scene_count,
            "sessionGenerationUnchanged": state.program_session_generation == before_generation,
        },
    }


def _inspection_request(args: argparse.Namespace) -> SimpleNamespace:
    return SimpleNamespace(
        include_subtrees=args.include_subtree,
        exclude_subtrees=args.exclude_subtree,
        probe_prims=args.probe_prim,
        include_hidden=args.include_hidden,
        include_cooked_convex=args.include_cooked_convex,
        include_cooked_samples=args.include_cooked_samples,
        max_colliders=args.max_colliders,
        purpose=[],
        output_profile=args.output_profile,
        timeout_s=args.timeout,
        request_topic=None,
        response_topic=None,
        node_name="manyforge_assistant_scene_authoring_smoke",
    )


def _smoke_result(
    *,
    mode: str,
    transport: str,
    report: dict[str, Any],
    propose_data: dict[str, Any],
    inspect_mutated: bool | None = False,
    workflow: str = "tools",
    lifecycle_data: dict[str, Any] | None = None,
) -> dict[str, Any]:
    result = {
        "success": True,
        "workflow": workflow,
        "mode": mode,
        "transport": transport,
        "inspect": {
            "summary": report.get("summary", {}),
            "warnings": report.get("warnings", []),
            "mutated": inspect_mutated,
        },
        "propose": {
            "proposalCount": len(propose_data.get("proposals", [])),
            "sceneResources": propose_data.get("sceneResources", []),
            "warnings": propose_data.get("warnings", []),
            "mutated": propose_data.get("mutated"),
        },
    }
    if lifecycle_data is not None:
        result["proposalLifecycle"] = lifecycle_data
    return result


def _checked_response(response: Any, label: str) -> dict[str, Any]:
    try:
        payload = response.json()
    except Exception as exc:
        raise RuntimeError(f"{label} returned non-JSON response: {exc}") from exc
    if response.status_code != 200:
        raise RuntimeError(f"{label} failed with status {response.status_code}: {payload}")
    if payload.get("mutated") is not False:
        raise RuntimeError(f"{label} must not mutate Composer state")
    return payload


def _load_fixture(path: str) -> dict[str, Any]:
    fixture_path = Path(path).expanduser().resolve()
    data = json.loads(fixture_path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise RuntimeError("fixture must contain a JSON object")
    if not isinstance(data.get("objects"), list):
        raise RuntimeError("fixture must contain an objects list")
    return data


def _write_minimal_program(directory: Path) -> Path:
    program_path = directory / "assistant_smoke_minimal.program.yaml"
    program_path.write_text(
        "\n".join(
            [
                "api_version: manyforge.program.v0",
                "program_profile: v0.0",
                "name: assistant_smoke_minimal",
                'catalog_version: "0.4.0"',
                "description: Minimal program for assistant proposal lifecycle smoke.",
                "robot:",
                "  robot_id: ur5",
                "  group_id: arm",
                "  base_frame: base_link",
                "  tcp_frame: tool0",
                "tree:",
                "  id: reset_trajectories",
                "  name: reset_trajectories",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return program_path


def _filter_report_for_request(report: dict[str, Any], request: Any) -> dict[str, Any]:
    filtered = copy.deepcopy(report)
    include_subtrees = tuple(getattr(request, "include_subtrees", ()) or ())
    exclude_subtrees = tuple(getattr(request, "exclude_subtrees", ()) or ())
    objects = []
    for obj in filtered.get("objects") or []:
        if not isinstance(obj, dict):
            continue
        source_root = str(obj.get("source_root") or "")
        if include_subtrees and not any(_path_is_under(source_root, item) for item in include_subtrees):
            continue
        if exclude_subtrees and any(_path_is_under(source_root, item) for item in exclude_subtrees):
            continue
        objects.append(obj)
    filtered["objects"] = objects
    summary = dict(filtered.get("summary") or {})
    summary["object_count"] = len(objects)
    summary["collider_count"] = sum(
        int(obj.get("collider_count") or 0)
        for obj in objects
        if isinstance(obj, dict)
    )
    summary["error_count"] = len(filtered.get("errors") or [])
    filtered["summary"] = summary
    return filtered


def _path_is_under(path: str, prefix: str) -> bool:
    normalized = prefix.rstrip("/")
    return path == normalized or path.startswith(normalized + "/")


if __name__ == "__main__":
    raise SystemExit(main())
