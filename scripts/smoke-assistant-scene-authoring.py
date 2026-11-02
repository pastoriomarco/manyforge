#!/usr/bin/env python3
"""Ask the active assistant to add one unique object to the Program Draft.

Only ``scene_draft_add_object`` is offered to the model. The script checks the
resulting Draft and confirms Saved and Applied identities did not change. It
does not Apply, Discard, Save, run a Program, or send a robot command. The
operator reviews the Draft and chooses Apply or Discard in Composer.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import sys
from types import ModuleType
from typing import Any
import uuid


ADD_TOOL = "scene_draft_add_object"


def _load_provider_smoke() -> ModuleType:
    path = Path(__file__).with_name("smoke-assistant-provider-bridge.py")
    spec = importlib.util.spec_from_file_location(
        "manyforge_assistant_provider_smoke_shared", path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load shared smoke client from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


_provider = _load_provider_smoke()
SmokeFailure = _provider.SmokeFailure


def _workspace(composer_url: str, *, timeout_s: float) -> dict[str, Any]:
    value = _provider.composer_get(
        composer_url,
        "/api/program/workspace",
        timeout_s=timeout_s,
    )
    draft = value.get("draft")
    if (
        type(draft) is not dict
        or draft.get("programLoaded") is not True
        or type(draft.get("authoring")) is not dict
    ):
        raise SmokeFailure("load a Program Draft in Composer before running the authoring smoke")
    return value


def _saved_applied_identity(workspace: dict[str, Any]) -> tuple[Any, ...]:
    saved = workspace.get("saved")
    applied = workspace.get("applied")
    if type(saved) is not dict or type(applied) is not dict:
        raise SmokeFailure("Composer workspace omitted Saved or Applied identity")
    return (
        saved.get("available"),
        saved.get("path"),
        saved.get("programRevision"),
        applied.get("available"),
        applied.get("programRevision"),
        applied.get("unavailableReason"),
    )


def _scene_ids(workspace: dict[str, Any]) -> set[str]:
    draft = workspace["draft"]
    authoring = draft["authoring"]
    scene = authoring.get("scene", [])
    if type(scene) is not list:
        raise SmokeFailure("Program Draft scene is not an array")
    return {
        item["scene_resource_id"]
        for item in scene
        if type(item) is dict and type(item.get("scene_resource_id")) is str
    }


def _resource(scene_resource_id: str) -> dict[str, Any]:
    return {
        "scene_resource_id": scene_resource_id,
        "shape": {
            "type": "box",
            "box_dimensions_m": [0.08, 0.04, 0.03],
        },
        "pose": {
            "frame_ref": {"kind": "cell_frame"},
            "position_m": [0.45, -0.1, 0.015],
            "orientation": {"rpy_deg": [0.0, 0.0, 0.0]},
        },
    }


def _verify_completed_response(
    status: dict[str, Any],
    request: dict[str, Any],
    resource: dict[str, Any],
) -> list[str]:
    errors: list[str] = []
    try:
        response = _provider.completed_response(
            status,
            request,
            require_draft_mutation=True,
        )
    except SmokeFailure as error:
        return [str(error)]
    calls = response.get("toolCalls")
    if type(calls) is not list or len(calls) != 1:
        return ["expected exactly one Draft tool call"]
    call = calls[0]
    if type(call) is not dict:
        return ["assistant tool call is not an object"]
    if call.get("toolId") != ADD_TOOL:
        errors.append(f"expected {ADD_TOOL}, got {call.get('toolId')!r}")
    if call.get("status") != "completed":
        errors.append("scene_draft_add_object did not complete")
    if call.get("effect") != "composer_draft_mutating":
        errors.append("scene tool call was not classified as a Draft mutation")
    if call.get("draftMutated") is not True:
        errors.append("scene tool call did not report Draft mutation")
    if call.get("arguments") != {"scene_resource": resource}:
        errors.append("model arguments did not match the requested scene resource")
    result = call.get("result")
    details = result.get("details") if type(result) is dict else None
    if (
        type(details) is not dict
        or details.get("inserted") != [resource["scene_resource_id"]]
    ):
        errors.append("Composer did not confirm insertion of the requested resource")
    return errors


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--composer-url",
        default=os.environ.get("MANYFORGE_COMPOSER_URL", "http://127.0.0.1:3000"),
        help="Composer API root (default: %(default)s)",
    )
    parser.add_argument(
        "--expected-component-id",
        help="optionally require this component for the chat alias",
    )
    parser.add_argument(
        "--timeout-seconds",
        type=float,
        default=180.0,
        help="maximum time to reconcile the assistant request (default: %(default)s)",
    )
    parser.add_argument(
        "--confirm-draft-only",
        action="store_true",
        help="confirm that one assistant tool may edit the in-memory Program Draft",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    if not args.confirm_draft_only:
        print(
            "Refusing to submit a Draft edit without --confirm-draft-only.",
            file=sys.stderr,
        )
        return 2
    if not 1.0 <= args.timeout_seconds <= 900.0:
        print("--timeout-seconds must be between 1 and 900", file=sys.stderr)
        return 2

    http_timeout_s = min(15.0, args.timeout_seconds)
    request: dict[str, Any] | None = None
    before: dict[str, Any] | None = None
    resource: dict[str, Any] | None = None
    status: dict[str, Any] | None = None
    submission_error: Exception | None = None
    try:
        target = _provider.preflight(
            args.composer_url,
            timeout_s=http_timeout_s,
            expected_component_id=args.expected_component_id,
        )
        before = _workspace(args.composer_url, timeout_s=http_timeout_s)
        scene_ids = _scene_ids(before)
        while True:
            candidate_id = f"manyforge_smoke_{uuid.uuid4().hex[:12]}"
            if candidate_id not in scene_ids:
                break
        resource = _resource(candidate_id)
        resource_json = _provider.json.dumps(
            resource,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        )
        request = _provider.make_request(
            message=(
                f"Call {ADD_TOOL} exactly once with this exact scene_resource JSON: "
                f"{resource_json}. Do not call any other tool. Do not Apply, Save, "
                "run a Program, or command a robot. After the tool call, briefly "
                f"confirm the Draft object ID {candidate_id}."
            ),
            requested_tools=[ADD_TOOL],
        )
        try:
            status = _provider.submit_and_wait(
                args.composer_url,
                request,
                timeout_s=args.timeout_seconds,
                http_timeout_s=http_timeout_s,
            )
        except SmokeFailure as error:
            submission_error = error

        after = _workspace(args.composer_url, timeout_s=http_timeout_s)
        before_draft = before["draft"]
        after_draft = after["draft"]
        verification_errors = []
        if _saved_applied_identity(before) != _saved_applied_identity(after):
            verification_errors.append("Saved or Applied identity changed during the smoke")
        if (
            type(before_draft.get("draftRevision")) is not int
            or type(after_draft.get("draftRevision")) is not int
            or after_draft["draftRevision"] <= before_draft["draftRevision"]
        ):
            verification_errors.append("Composer Draft revision did not advance")
        after_scene = after_draft["authoring"].get("scene", [])
        if not isinstance(after_scene, list) or resource is None or not any(
            type(item) is dict
            and item.get("scene_resource_id") == resource["scene_resource_id"]
            and item == resource
            for item in after_scene
        ):
            verification_errors.append("expected scene resource is absent from the current Draft")
        if resource is not None:
            added_ids = _scene_ids(after) - scene_ids
            expected_ids = {resource["scene_resource_id"]}
            if added_ids != expected_ids:
                verification_errors.append(
                    f"unexpected Draft scene additions: {sorted(added_ids)}"
                )
        if submission_error is not None:
            raise SmokeFailure(
                f"request {request['requestId']} did not reach a known completed status: "
                f"{submission_error}"
            )
        assert request is not None and status is not None and resource is not None
        verification_errors.extend(
            _verify_completed_response(status, request, resource)
        )
        if verification_errors:
            raise SmokeFailure(
                f"request {request['requestId']} may have edited the Draft; "
                "inspect it in Composer before retrying: "
                + "; ".join(verification_errors)
            )
        print(
            "PASS: Draft-only assistant scene edit; "
            f"component={target.component_id} model={target.model_id} "
            f"object={resource['scene_resource_id']} "
            f"request={request['requestId']}"
        )
        print("Review the Program Draft in Composer, then choose Apply or Discard.")
        return 0
    except (SmokeFailure, OSError, ValueError) as error:
        detail = str(error)
        if request is not None and before is not None:
            detail += (
                f"\nRequest {request['requestId']} was submitted at most once. "
                "Inspect the Draft and request status before retrying."
            )
        print(f"FAIL: {detail}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
