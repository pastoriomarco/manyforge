#!/usr/bin/env python3
"""Live ManyForge assistant e2e smoke and sampling-profile probe.

This script expects a live vLLM-compatible model endpoint and a live Composer
backend. It starts the ManyForge assistant bridge once per sampling profile so
profile behavior is repeatable and not hidden in a manually-started process.

The script is intentionally stdlib-only so it can run from the host.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


REQUEST_VERSION = "manyforge.assistant.provider_request.v0"
DEFAULT_MODEL = "cosmos-reason2-8b"
DEFAULT_PROFILES = [
    "vllm-generation-config",
    "cosmos-bridge-default",
]


@dataclasses.dataclass(frozen=True)
class Profile:
    name: str
    bridge_env: dict[str, str]
    upstream_body: dict[str, Any]
    start_bridge: bool = True
    expect_e2e_success: bool = False
    note: str = ""


PROFILES: dict[str, Profile] = {
    "vllm-generation-config": Profile(
        name="vllm-generation-config",
        bridge_env={},
        upstream_body={},
        start_bridge=False,
        note="Direct vLLM probe with no per-request generation overrides.",
    ),
    "bridge-default": Profile(
        name="bridge-default",
        bridge_env={},
        upstream_body={"temperature": 0.0, "max_tokens": 512},
        note="Bridge defaults: deterministic, capped output, thinking unset.",
    ),
    "cosmos-bridge-default": Profile(
        name="cosmos-bridge-default",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "8192",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.0",
        },
        upstream_body={"temperature": 0.0, "max_tokens": 8192},
        expect_e2e_success=True,
        note="Cosmos Reason2 8B bridge smoke profile; uses NemoClaw-Thor serving defaults plus a bounded output cap.",
    ),
    "omni-vendor-tool": Profile(
        name="omni-vendor-tool",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "512",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.6",
            "BRIDGE_UPSTREAM_TOP_P": "0.95",
        },
        upstream_body={"temperature": 0.6, "top_p": 0.95, "max_tokens": 512},
        note="NVIDIA tool-call sampling, thinking unset.",
    ),
    "omni-vendor-tool-no-think": Profile(
        name="omni-vendor-tool-no-think",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "512",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.6",
            "BRIDGE_UPSTREAM_TOP_P": "0.95",
            "BRIDGE_UPSTREAM_ENABLE_THINKING": "false",
        },
        upstream_body={
            "temperature": 0.6,
            "top_p": 0.95,
            "max_tokens": 512,
            "chat_template_kwargs": {"enable_thinking": False},
        },
        note="NVIDIA tool-call sampling with reasoning disabled.",
    ),
    "omni-vendor-tool-no-think-32768": Profile(
        name="omni-vendor-tool-no-think-32768",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "32768",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.6",
            "BRIDGE_UPSTREAM_TOP_P": "0.95",
            "BRIDGE_UPSTREAM_ENABLE_THINKING": "false",
        },
        upstream_body={
            "temperature": 0.6,
            "top_p": 0.95,
            "max_tokens": 32768,
            "chat_template_kwargs": {"enable_thinking": False},
        },
        note="NVIDIA tool-call sampling with reasoning disabled and a 32768-token cap.",
    ),
    "omni-instruct-no-think": Profile(
        name="omni-instruct-no-think",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "512",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.2",
            "BRIDGE_UPSTREAM_TOP_K": "1",
            "BRIDGE_UPSTREAM_ENABLE_THINKING": "false",
        },
        upstream_body={
            "temperature": 0.2,
            "top_k": 1,
            "max_tokens": 512,
            "chat_template_kwargs": {"enable_thinking": False},
        },
        expect_e2e_success=True,
        note="NVIDIA non-thinking instruct profile; current recommended smoke profile.",
    ),
    "omni-instruct-no-think-128": Profile(
        name="omni-instruct-no-think-128",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "128",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.2",
            "BRIDGE_UPSTREAM_TOP_K": "1",
            "BRIDGE_UPSTREAM_ENABLE_THINKING": "false",
        },
        upstream_body={
            "temperature": 0.2,
            "top_k": 1,
            "max_tokens": 128,
            "chat_template_kwargs": {"enable_thinking": False},
        },
        note="Output-budget sweep: recommended profile with a 128-token cap.",
    ),
    "omni-instruct-no-think-256": Profile(
        name="omni-instruct-no-think-256",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "256",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.2",
            "BRIDGE_UPSTREAM_TOP_K": "1",
            "BRIDGE_UPSTREAM_ENABLE_THINKING": "false",
        },
        upstream_body={
            "temperature": 0.2,
            "top_k": 1,
            "max_tokens": 256,
            "chat_template_kwargs": {"enable_thinking": False},
        },
        note="Output-budget sweep: recommended profile with a 256-token cap.",
    ),
    "omni-instruct-no-think-1024": Profile(
        name="omni-instruct-no-think-1024",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "1024",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.2",
            "BRIDGE_UPSTREAM_TOP_K": "1",
            "BRIDGE_UPSTREAM_ENABLE_THINKING": "false",
        },
        upstream_body={
            "temperature": 0.2,
            "top_k": 1,
            "max_tokens": 1024,
            "chat_template_kwargs": {"enable_thinking": False},
        },
        note="Output-budget sweep: recommended profile with a 1024-token cap.",
    ),
    "omni-instruct-no-think-32768": Profile(
        name="omni-instruct-no-think-32768",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "32768",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.2",
            "BRIDGE_UPSTREAM_TOP_K": "1",
            "BRIDGE_UPSTREAM_ENABLE_THINKING": "false",
        },
        upstream_body={
            "temperature": 0.2,
            "top_k": 1,
            "max_tokens": 32768,
            "chat_template_kwargs": {"enable_thinking": False},
        },
        note="Output-budget sweep: recommended profile with a 32768-token cap.",
    ),
    "omni-deterministic-no-think": Profile(
        name="omni-deterministic-no-think",
        bridge_env={
            "BRIDGE_UPSTREAM_MAX_TOKENS": "512",
            "BRIDGE_UPSTREAM_TEMPERATURE": "0.0",
            "BRIDGE_UPSTREAM_TOP_K": "1",
            "BRIDGE_UPSTREAM_ENABLE_THINKING": "false",
        },
        upstream_body={
            "temperature": 0.0,
            "top_k": 1,
            "max_tokens": 512,
            "chat_template_kwargs": {"enable_thinking": False},
        },
        note="Deterministic non-thinking comparison.",
    ),
}


class SmokeFailure(RuntimeError):
    """Raised when a required smoke step fails."""


def main() -> int:
    args = _parse_args()
    selected_profiles = _selected_profiles(args.profile)
    results: dict[str, Any] = {
        "ok": True,
        "startedAt": time.time(),
        "profiles": {},
        "negativeCases": {},
        "recommendedProfile": "cosmos-bridge-default",
    }

    try:
        model = args.model or _resolve_model(args.upstream_base_url) or DEFAULT_MODEL
        _check_health(args.composer_url)
        _load_baseline(args)

        for profile in selected_profiles:
            results["profiles"][profile.name] = _run_profile(args, profile, model)

        results["negativeCases"] = _run_negative_cases(args, model)
    except Exception as exc:
        results["ok"] = False
        results["error"] = str(exc)

    results["finishedAt"] = time.time()
    results["durationS"] = round(results["finishedAt"] - results["startedAt"], 3)

    if args.output:
        Path(args.output).write_text(json.dumps(results, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(results, indent=2, sort_keys=True))

    if args.strict and not _strict_success(results):
        return 1
    return 0 if results.get("ok") else 2


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--composer-url", default="http://127.0.0.1:9000")
    parser.add_argument("--bridge-url", default="http://127.0.0.1:8100")
    parser.add_argument("--upstream-base-url", default="http://127.0.0.1:8000/v1")
    parser.add_argument("--model", default="")
    parser.add_argument(
        "--deployment-path",
        default="/workspace/examples/assistant_modes_scene_authoring.deployment.yaml",
    )
    parser.add_argument(
        "--program-path",
        default="/workspace/examples/pick_and_place_ur10e_robotiq.program.yaml",
    )
    parser.add_argument("--bridge-host", default="127.0.0.1")
    parser.add_argument("--bridge-port", type=int, default=8100)
    parser.add_argument("--provider-id", default="nemoclaw")
    parser.add_argument(
        "--bridge-python",
        default="",
        help="Python executable used to launch the bridge. Defaults to the bridge venv if present.",
    )
    parser.add_argument("--profile", action="append", default=[])
    parser.add_argument("--case-timeout-s", type=float, default=180.0)
    parser.add_argument("--direct-timeout-s", type=float, default=120.0)
    parser.add_argument("--bridge-start-timeout-s", type=float, default=20.0)
    parser.add_argument("--bridge-request-timeout-s", type=float, default=180.0)
    parser.add_argument("--bridge-max-wall-time-s", type=float, default=180.0)
    parser.add_argument("--bridge-max-turns", type=int, default=16)
    parser.add_argument("--log-dir", default="/tmp/manyforge-assistant-e2e")
    parser.add_argument("--skip-direct-vllm", action="store_true")
    parser.add_argument("--skip-e2e", action="store_true")
    parser.add_argument("--skip-negative", action="store_true")
    parser.add_argument(
        "--require-hmi",
        action="store_true",
        help="Fail operator-flow checks unless Composer reports a healthy HMI/Viser.",
    )
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--output", default="")
    return parser.parse_args()


def _selected_profiles(names: list[str]) -> list[Profile]:
    requested = names or list(DEFAULT_PROFILES)
    if requested == ["all"]:
        requested = list(PROFILES)
    unknown = sorted(set(requested) - set(PROFILES))
    if unknown:
        raise SmokeFailure(f"unknown profile(s): {unknown}; available: {sorted(PROFILES)}")
    return [PROFILES[name] for name in requested]


def _run_profile(args: argparse.Namespace, profile: Profile, model: str) -> dict[str, Any]:
    result: dict[str, Any] = {"profile": profile.name, "note": profile.note}
    if not args.skip_direct_vllm:
        result["directVllm"] = _direct_vllm_probe(args, profile, model)

    if args.skip_e2e or not profile.start_bridge:
        return result

    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    audit_path = log_dir / f"{profile.name}-audit.jsonl"
    bridge_log = log_dir / f"{profile.name}-bridge.log"
    with _bridge_process(args, profile, model, audit_path, bridge_log):
        _load_baseline(args)
        result["queryReadOnly"] = _query_read_only_case(args)
        _load_baseline(args)
        result["operatorComposerFlow"] = _operator_composer_flow_case(args, profile.name)
        _load_baseline(args)
        result["composerDraftAdd"] = _composer_draft_add_case(args, profile.name)
        _load_baseline(args)
        result["goldenSceneUpsert"] = _golden_scene_upsert_case(args, profile.name)
        _load_baseline(args)
        result["goldenTreeInsert"] = _golden_tree_insert_case(args, profile.name)
        _load_baseline(args)
        result["goldenParameterUpsert"] = _golden_parameter_upsert_case(args, profile.name)
        _load_baseline(args)
        result["goldenBlackboardUpsert"] = _golden_blackboard_upsert_case(args, profile.name)
        _load_baseline(args)
        result["draftToolSurface"] = _draft_tool_surface_case(args, profile.name)
        _load_baseline(args)
        result["cancelProxy"] = _cancel_proxy_case(args, profile.name)
        result["auditLog"] = str(audit_path)
        result["bridgeLog"] = str(bridge_log)
    return result


def _run_negative_cases(args: argparse.Namespace, model: str) -> dict[str, Any]:
    if args.skip_negative:
        return {"skipped": True}
    profile = PROFILES["cosmos-bridge-default"]
    log_dir = Path(args.log_dir)
    log_dir.mkdir(parents=True, exist_ok=True)
    audit_path = log_dir / "negative-audit.jsonl"
    bridge_log = log_dir / "negative-bridge.log"
    with _bridge_process(args, profile, model, audit_path, bridge_log):
        _load_baseline(args)
        cases = {
            "missingAssistantMode": _expect_http_error(
                "Composer rejects provider request without assistantMode",
                _post_json,
                f"{args.composer_url}/api/assistant/chat",
                {
                    "message": "read the program",
                    "mode": "provider",
                    "providerId": args.provider_id,
                },
                timeout_s=args.case_timeout_s,
                expected_status=400,
            ),
            "requestedToolOutsideMode": _expect_http_error(
                "Composer rejects requested tool outside query mode",
                _post_json,
                f"{args.composer_url}/api/assistant/chat",
                {
                    "message": "add an object",
                    "mode": "provider",
                    "providerId": args.provider_id,
                    "assistantMode": "query",
                    "requestedTools": ["scene_draft_add_object"],
                },
                timeout_s=args.case_timeout_s,
                expected_status=400,
            ),
            "invalidDraftCallbackArgs": _expect_http_error(
                "Composer callback rejects invalid sceneResource",
                _post_json,
                f"{args.composer_url}/api/assistant/bridge/tools/scene_draft_add_object",
                {
                    "requestId": "negative-invalid-draft",
                    "assistantMode": "composer-assistant",
                    "catalogHash": _active_mode_catalog_hash(args, "composer-assistant"),
                    "arguments": {"sceneResource": {"objectId": "missing_shape_pose"}},
                },
                timeout_s=args.case_timeout_s,
                expected_status=400,
            ),
            "bridgeMissingAssistantMode": _expect_http_error(
                "Bridge rejects envelope without assistantMode",
                _post_json,
                f"{args.bridge_url}/v1/manyforge/assistant",
                {
                    "version": REQUEST_VERSION,
                    "schemaVersion": "0.1.0",
                    "requestId": "negative-missing-mode",
                    "providerId": args.provider_id,
                    "message": "hello",
                    "catalog": {"skillCatalog": [], "nodeCatalog": []},
                    "tools": [],
                },
                timeout_s=args.case_timeout_s,
                expected_status=400,
            ),
            "bridgeRejectsQuerySkills": _expect_http_error(
                "Bridge rejects query catalog with skills",
                _post_json,
                f"{args.bridge_url}/v1/manyforge/assistant",
                {
                    "version": REQUEST_VERSION,
                    "schemaVersion": "0.1.0",
                    "requestId": "negative-query-skill",
                    "providerId": args.provider_id,
                    "assistantMode": "query",
                    "message": "hello",
                    "catalog": {"skillCatalog": [{"id": "some.skill"}], "nodeCatalog": []},
                    "tools": [],
                },
                timeout_s=args.case_timeout_s,
                expected_status=400,
            ),
            "composerStructuredCancelledError": _composer_cancelled_error_case(args),
        }
    cases["auditLog"] = str(audit_path)
    cases["bridgeLog"] = str(bridge_log)
    return cases


@contextlib.contextmanager
def _bridge_process(
    args: argparse.Namespace,
    profile: Profile,
    model: str,
    audit_path: Path,
    bridge_log: Path,
):
    python = _bridge_python(args)
    env = os.environ.copy()
    env.update(
        {
            "BRIDGE_HOST": args.bridge_host,
            "BRIDGE_PORT": str(args.bridge_port),
            "BRIDGE_UPSTREAM_BASE_URL": args.upstream_base_url,
            "BRIDGE_MANYFORGE_BASE_URL": args.composer_url,
            "BRIDGE_MODEL": model,
            "BRIDGE_REQUEST_TIMEOUT_S": str(args.bridge_request_timeout_s),
            "BRIDGE_MAX_WALL_TIME_S": str(args.bridge_max_wall_time_s),
            "BRIDGE_MAX_TURNS": str(args.bridge_max_turns),
            "BRIDGE_AUDIT_LOG": str(audit_path),
        }
    )
    env.update(profile.bridge_env)
    _stop_existing_bridge(args.bridge_port)
    with bridge_log.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            [str(python), "-m", "manyforge_assistant_bridge.bridge"],
            cwd=str(_repo_root()),
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
    try:
        _wait_for_bridge(args, process)
        yield process
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def _bridge_python(args: argparse.Namespace) -> Path:
    if args.bridge_python:
        return Path(args.bridge_python)
    venv_python = _repo_root() / "manyforge_assistant_bridge" / ".venv" / "bin" / "python"
    if venv_python.exists():
        return venv_python
    return Path(sys.executable)


def _wait_for_bridge(args: argparse.Namespace, process: subprocess.Popen[str]) -> None:
    deadline = time.monotonic() + args.bridge_start_timeout_s
    last_error = ""
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise SmokeFailure(f"bridge exited early with code {process.returncode}")
        try:
            health = _get_json(f"{args.bridge_url}/healthz", timeout_s=2.0)
            if health.get("status") == "ok":
                return
        except Exception as exc:  # noqa: BLE001 - diagnostic only
            last_error = str(exc)
        time.sleep(0.5)
    raise SmokeFailure(f"bridge did not become healthy: {last_error}")


def _stop_existing_bridge(port: int) -> None:
    try:
        health = _get_json(f"http://127.0.0.1:{port}/healthz", timeout_s=0.5)
    except Exception:
        return
    if health.get("version") == REQUEST_VERSION:
        subprocess.run(
            ["pkill", "-f", "manyforge_assistant_bridge.bridge"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        time.sleep(1.0)


def _direct_vllm_probe(args: argparse.Namespace, profile: Profile, model: str) -> dict[str, Any]:
    body = {
        "model": model,
        "messages": [
            {
                "role": "user",
                "content": _add_object_prompt("direct_probe_block"),
            }
        ],
        "tools": [_scene_draft_add_tool_descriptor()],
        "tool_choice": "auto",
    }
    body.update(profile.upstream_body)
    started = time.perf_counter()
    try:
        payload = _post_json(
            f"{args.upstream_base_url.rstrip('/')}/chat/completions",
            body,
            timeout_s=args.direct_timeout_s,
        )
        message = ((payload.get("choices") or [{}])[0].get("message") or {})
        tool_calls = message.get("tool_calls") or []
        return {
            "ok": True,
            "durationS": round(time.perf_counter() - started, 3),
            "toolCallCount": len(tool_calls) if isinstance(tool_calls, list) else 0,
            "toolCallNames": _tool_call_names(tool_calls),
            "contentEmpty": not bool(message.get("content")),
            "reasoningPresent": bool(message.get("reasoning") or message.get("reasoning_content")),
        }
    except Exception as exc:  # noqa: BLE001 - report probe result
        return {
            "ok": False,
            "durationS": round(time.perf_counter() - started, 3),
            "error": str(exc),
        }


def _query_read_only_case(args: argparse.Namespace) -> dict[str, Any]:
    request_id = f"smoke-query-{int(time.time() * 1000)}"
    payload = {
        "requestId": request_id,
        "message": "Call program_read exactly once, then summarize whether a program is loaded.",
        "mode": "provider",
        "providerId": args.provider_id,
        "assistantMode": "query",
        "requestedTools": ["program_read"],
    }
    started = time.perf_counter()
    try:
        data = _post_json(
            f"{args.composer_url}/api/assistant/chat",
            payload,
            timeout_s=args.case_timeout_s,
        )
        calls = data.get("toolCalls") or []
        completed = [
            call
            for call in calls
            if isinstance(call, dict)
            and call.get("name") == "program_read"
            and call.get("status") == "completed"
        ]
        status = _request_status(args, request_id)
        status_ok = (
            status.get("status") == "completed"
            and status.get("requestId") == request_id
            and status.get("error") is None
        )
        return {
            "ok": bool(completed) and status_ok,
            "requestId": request_id,
            "durationS": round(time.perf_counter() - started, 3),
            "message": data.get("message", "")[:240],
            "toolCalls": _compact_tool_calls(calls),
            "requestStatus": status,
        }
    except Exception as exc:  # noqa: BLE001
        return {"ok": False, "durationS": round(time.perf_counter() - started, 3), "error": str(exc)}


def _composer_draft_add_case(args: argparse.Namespace, profile_name: str) -> dict[str, Any]:
    object_id = f"assistant_smoke_{_slug(profile_name)}"
    request_id = f"smoke-draft-{_slug(profile_name)}-{int(time.time() * 1000)}"
    payload = {
        "requestId": request_id,
        "message": _add_object_prompt(object_id),
        "mode": "provider",
        "providerId": args.provider_id,
        "assistantMode": "composer-assistant",
        "requestedTools": ["scene_draft_add_object"],
    }
    started = time.perf_counter()
    try:
        data = _post_json(
            f"{args.composer_url}/api/assistant/chat",
            payload,
            timeout_s=args.case_timeout_s,
        )
        scene_ids = _scene_object_ids(data)
        ok = (
            data.get("draftMutated") is True
            and object_id in scene_ids
            and any(
                isinstance(call, dict)
                and call.get("name") == "scene_draft_add_object"
                and call.get("status") == "completed"
                for call in (data.get("toolCalls") or [])
            )
        )
        status = _request_status(args, request_id)
        status_ok = (
            status.get("status") == "completed"
            and status.get("requestId") == request_id
            and status.get("draftMutated") is True
            and status.get("error") is None
        )
        discard = _discard_draft_by_reload(args, object_id)
        return {
            "ok": ok and status_ok,
            "requestId": request_id,
            "durationS": round(time.perf_counter() - started, 3),
            "draftMutated": data.get("draftMutated"),
            "objectId": object_id,
            "sceneContainsObject": object_id in scene_ids,
            "toolCalls": _compact_tool_calls(data.get("toolCalls") or []),
            "requestStatus": status,
            "discardByReload": discard,
            "message": data.get("message", "")[:240],
        }
    except Exception as exc:  # noqa: BLE001
        _load_baseline(args)
        return {"ok": False, "durationS": round(time.perf_counter() - started, 3), "error": str(exc)}


def _operator_composer_flow_case(args: argparse.Namespace, profile_name: str) -> dict[str, Any]:
    """Exercise the UI-facing Composer Assistant flow through public HTTP APIs."""
    slug = _slug(profile_name)
    resource = {
        "objectId": f"ui_demo_box_{slug}",
        "shape": {"type": "box", "box_dimensions_m": [0.04, 0.04, 0.04]},
        "pose": {
            "frame_id": "universe",
            "position_m": [0.42, -0.12, 0.02],
            "orientation_rpy_deg": [0.0, 0.0, 0.0],
        },
    }
    request_id = f"smoke-operator-flow-{slug}-{int(time.time() * 1000)}"
    payload = {
        "requestId": request_id,
        "message": (
            "Call scene_draft_upsert_objects exactly once to add this Composer draft "
            f"scene object, with no extra objects: {json.dumps(resource, separators=(',', ':'))}. "
            "Then briefly report that the normal Composer draft controls own apply or discard."
        ),
        "mode": "provider",
        "providerId": args.provider_id,
        "assistantMode": "composer-assistant",
        "requestedTools": ["scene_draft_upsert_objects"],
    }
    started = time.perf_counter()
    try:
        providers = _get_json(f"{args.composer_url}/api/assistant/providers", timeout_s=args.case_timeout_s)
        infra_before = _get_json(f"{args.composer_url}/api/infra/status", timeout_s=args.case_timeout_s)
        hmi_status = _hmi_status(infra_before)
        validation_errors: list[str] = []
        if providers.get("configuredProviderId") != args.provider_id:
            validation_errors.append(
                f"configuredProviderId expected {args.provider_id!r}, got {providers.get('configuredProviderId')!r}"
            )
        if args.require_hmi and hmi_status.get("status") != "healthy":
            validation_errors.append(f"HMI required but unavailable: {hmi_status}")

        data = _post_json(
            f"{args.composer_url}/api/assistant/chat",
            payload,
            timeout_s=args.case_timeout_s,
        )
        calls = data.get("toolCalls") or []
        allowed_tools = _active_mode_tools(args, "composer-assistant")
        validation_errors.extend(_out_of_mode_tool_errors(calls, allowed_tools, label="toolCalls"))
        completed, tool_errors = _single_completed_tool_call(calls, "scene_draft_upsert_objects")
        validation_errors.extend(tool_errors)
        argument_resources = _tool_call_scene_resources(completed)
        validation_errors.extend(
            _scene_resource_set_match_errors(
                argument_resources,
                [resource],
                label="toolCalls[0].arguments.sceneResources",
                require_exact=True,
            )
        )

        program = _get_json(f"{args.composer_url}/api/program", timeout_s=args.case_timeout_s)
        scene_resources = [
            item
            for item in (program.get("sceneResources") or [])
            if isinstance(item, dict)
        ]
        scene_by_id = {item.get("objectId"): item for item in scene_resources}
        validation_errors.extend(
            _scene_resource_set_match_errors(
                [scene_by_id.get(resource["objectId"])],
                [resource],
                label="program.sceneResources",
                require_exact=False,
            )
        )

        diagnostics = _scene_diagnostic_for_object(args, resource["objectId"], hmi_status)
        if diagnostics.get("checked"):
            if not diagnostics.get("present"):
                validation_errors.append(f"scene diagnostics missing {resource['objectId']}")
            if diagnostics.get("hmiAvailable") and not diagnostics.get("match"):
                validation_errors.append(
                    f"HMI diagnostics mismatch for {resource['objectId']}: {diagnostics.get('mismatchReason')}"
                )

        status = _request_status(args, request_id)
        status_ok = _draft_request_status_ok(status, request_id)
        discard = _discard_draft_by_reload(args, resource["objectId"])
        return {
            "ok": (
                not validation_errors
                and status_ok
                and data.get("draftMutated") is True
                and discard.get("ok") is True
            ),
            "requestId": request_id,
            "durationS": round(time.perf_counter() - started, 3),
            "providerCatalog": {
                "configuredProviderId": providers.get("configuredProviderId"),
                "configuredProviderPresent": any(
                    isinstance(provider, dict)
                    and provider.get("id") == args.provider_id
                    and provider.get("configured") is True
                    for provider in providers.get("providers") or []
                ),
            },
            "hmi": hmi_status,
            "draftMutated": data.get("draftMutated"),
            "expectedObjectId": resource["objectId"],
            "sceneContainsObject": resource["objectId"] in scene_by_id,
            "diagnostics": diagnostics,
            "validationErrors": validation_errors,
            "toolCalls": _compact_tool_calls(calls),
            "requestStatus": status,
            "discardByReload": discard,
            "message": data.get("message", "")[:240],
        }
    except Exception as exc:  # noqa: BLE001
        _load_baseline(args)
        return {"ok": False, "durationS": round(time.perf_counter() - started, 3), "error": str(exc)}


def _golden_scene_upsert_case(args: argparse.Namespace, profile_name: str) -> dict[str, Any]:
    slug = _slug(profile_name)
    expected_resources = _golden_scene_resources(slug)
    expected_ids = [resource["objectId"] for resource in expected_resources]
    request_id = f"smoke-golden-{slug}-{int(time.time() * 1000)}"
    payload = {
        "requestId": request_id,
        "message": _golden_scene_prompt(expected_resources),
        "mode": "provider",
        "providerId": args.provider_id,
        "assistantMode": "composer-assistant",
        "requestedTools": ["scene_draft_upsert_objects"],
    }
    started = time.perf_counter()
    try:
        data = _post_json(
            f"{args.composer_url}/api/assistant/chat",
            payload,
            timeout_s=args.case_timeout_s,
        )
        calls = data.get("toolCalls") or []
        allowed_tools = _active_mode_tools(args, "composer-assistant")
        upsert_calls = [
            call
            for call in calls
            if isinstance(call, dict) and call.get("name") == "scene_draft_upsert_objects"
        ]
        completed = [
            call
            for call in upsert_calls
            if call.get("status") == "completed"
        ]
        scene_by_id = {
            resource.get("objectId") or resource.get("object_id"): resource
            for resource in _scene_resources(data)
            if isinstance(resource, dict)
        }
        validation_errors: list[str] = []
        if len(calls) != 1:
            validation_errors.append(f"expected exactly 1 tool call, got {len(calls)}")
        if len(upsert_calls) != 1:
            validation_errors.append(
                f"expected exactly 1 scene_draft_upsert_objects call, got {len(upsert_calls)}"
            )
        if len(completed) != 1:
            validation_errors.append(
                f"expected exactly 1 completed scene_draft_upsert_objects call, got {len(completed)}"
            )
        validation_errors.extend(_out_of_mode_tool_errors(calls, allowed_tools, label="toolCalls"))

        completed_call = completed[0] if len(completed) == 1 else {}
        argument_resources = _tool_call_scene_resources(completed_call)
        validation_errors.extend(
            _scene_resource_set_match_errors(
                argument_resources,
                expected_resources,
                label="toolCalls[0].arguments.sceneResources",
                require_exact=True,
            )
        )
        result = completed_call.get("result") if isinstance(completed_call, dict) else {}
        result_object_ids = _string_list((result or {}).get("objectIds")) if isinstance(result, dict) else []
        if isinstance(result, dict) and result.get("draftMutation") != "scene_resources_upsert":
            validation_errors.append(
                "toolCalls[0].result.draftMutation expected "
                f"'scene_resources_upsert', got {result.get('draftMutation')!r}"
            )
        validation_errors.extend(
            _exact_string_set_errors(
                result_object_ids,
                expected_ids,
                label="toolCalls[0].result.objectIds",
            )
        )
        validation_errors.extend(
            _scene_resource_set_match_errors(
                [scene_by_id.get(resource["objectId"]) for resource in expected_resources],
                expected_resources,
                label="toolCalls[0].result.scene",
                require_exact=False,
            )
        )
        program = _get_json(f"{args.composer_url}/api/program", timeout_s=args.case_timeout_s)
        program_scene_by_id = _program_scene_by_id(program)
        validation_errors.extend(
            _scene_resource_set_match_errors(
                [program_scene_by_id.get(resource["objectId"]) for resource in expected_resources],
                expected_resources,
                label="program.sceneResources",
                require_exact=False,
            )
        )
        validation_errors.extend(
            _unexpected_slugged_values_errors(
                list(program_scene_by_id),
                expected_ids,
                prefix="golden_",
                suffix=f"_{slug}",
                label="program.sceneResources.objectIds",
            )
        )
        validation_errors.extend(
            _message_mentions_errors(
                data.get("message"),
                expected_ids,
                label="message",
            )
        )
        status = _request_status(args, request_id)
        validation_errors.extend(
            _request_status_errors(
                status,
                request_id=request_id,
                assistant_mode="composer-assistant",
                provider_id=args.provider_id,
                draft_mutated=True,
                tool_call_count=1,
                label="requestStatus",
            )
        )
        discard = _discard_draft_by_reload(args, expected_ids[0])
        return {
            "ok": (
                not validation_errors
                and data.get("draftMutated") is True
                and discard.get("ok") is True
            ),
            "requestId": request_id,
            "durationS": round(time.perf_counter() - started, 3),
            "draftMutated": data.get("draftMutated"),
            "expectedObjectIds": expected_ids,
            "matchedExpectedObjects": [
                resource["objectId"]
                for resource in expected_resources
                if not _scene_resource_match_errors(
                    scene_by_id.get(resource["objectId"]),
                    resource,
                    label=f"toolCalls[0].result.scene[{resource['objectId']}]",
                )
            ],
            "actualUpsertObjectIds": result_object_ids,
            "programDraftObjectIds": sorted(program_scene_by_id),
            "toolCallCount": len(calls) if isinstance(calls, list) else 0,
            "validationErrors": validation_errors,
            "toolCalls": _compact_tool_calls(calls),
            "requestStatus": status,
            "discardByReload": discard,
            "message": data.get("message", "")[:240],
        }
    except Exception as exc:  # noqa: BLE001
        _load_baseline(args)
        return {"ok": False, "durationS": round(time.perf_counter() - started, 3), "error": str(exc)}


def _golden_tree_insert_case(args: argparse.Namespace, profile_name: str) -> dict[str, Any]:
    slug = _slug(profile_name)
    parent_name = "pick_and_place"
    expected_node = {
        "id": "remove_collision_object",
        "name": f"golden_tree_remove_{slug}",
        "params": {"object_id": f"golden_tree_object_{slug}"},
    }
    expected_arguments = {"parentName": parent_name, "index": 0, "node": expected_node}
    request_id = f"smoke-golden-tree-{slug}-{int(time.time() * 1000)}"
    payload = {
        "requestId": request_id,
        "message": _golden_tree_insert_prompt(expected_arguments),
        "mode": "provider",
        "providerId": args.provider_id,
        "assistantMode": "composer-assistant",
        "requestedTools": ["tree_draft_insert_node"],
    }
    started = time.perf_counter()
    try:
        data = _post_json(
            f"{args.composer_url}/api/assistant/chat",
            payload,
            timeout_s=args.case_timeout_s,
        )
        calls = data.get("toolCalls") or []
        allowed_tools = _active_mode_tools(args, "composer-assistant")
        completed, validation_errors = _single_completed_tool_call(
            calls,
            "tree_draft_insert_node",
        )
        validation_errors.extend(_out_of_mode_tool_errors(calls, allowed_tools, label="toolCalls"))
        arguments = completed.get("arguments") if completed else {}
        validation_errors.extend(
            _tree_insert_argument_errors(
                arguments if isinstance(arguments, dict) else {},
                expected_arguments,
                label="toolCalls[0].arguments",
            )
        )
        result = completed.get("result") if completed else {}
        result = result if isinstance(result, dict) else {}
        validation_errors.extend(
            _tree_insert_result_errors(
                result,
                expected_arguments,
                label="toolCalls[0].result",
            )
        )
        tree_response = _get_json(f"{args.composer_url}/api/program/tree", timeout_s=args.case_timeout_s)
        validation_errors.extend(
            _tree_insert_result_errors(
                {
                    "draftMutation": "tree_node_insert",
                    "nodeName": expected_node["name"],
                    "parentName": parent_name,
                    "index": expected_arguments["index"],
                    "tree": tree_response.get("tree"),
                },
                expected_arguments,
                label="program.tree",
            )
        )
        validation_errors.extend(
            _message_mentions_errors(
                data.get("message"),
                [expected_node["name"]],
                label="message",
            )
        )
        status = _request_status(args, request_id)
        validation_errors.extend(
            _request_status_errors(
                status,
                request_id=request_id,
                assistant_mode="composer-assistant",
                provider_id=args.provider_id,
                draft_mutated=True,
                tool_call_count=1,
                label="requestStatus",
            )
        )
        discard = _discard_tree_by_reload(args, expected_node["name"])
        return {
            "ok": (
                not validation_errors
                and data.get("draftMutated") is True
                and discard.get("ok") is True
            ),
            "requestId": request_id,
            "durationS": round(time.perf_counter() - started, 3),
            "draftMutated": data.get("draftMutated"),
            "expectedNode": expected_node,
            "toolCallCount": len(calls) if isinstance(calls, list) else 0,
            "validationErrors": validation_errors,
            "toolCalls": _compact_tool_calls(calls),
            "requestStatus": status,
            "discardByReload": discard,
            "message": data.get("message", "")[:240],
        }
    except Exception as exc:  # noqa: BLE001
        _load_baseline(args)
        return {"ok": False, "durationS": round(time.perf_counter() - started, 3), "error": str(exc)}


def _golden_parameter_upsert_case(args: argparse.Namespace, profile_name: str) -> dict[str, Any]:
    slug = _slug(profile_name)
    expected_parameter = {
        "name": f"golden_param_{slug}",
        "type": "string",
        "default": "golden_object",
        "description": "Assistant golden E2E parameter.",
    }
    request_id = f"smoke-golden-param-{slug}-{int(time.time() * 1000)}"
    payload = {
        "requestId": request_id,
        "message": _golden_parameter_upsert_prompt(expected_parameter),
        "mode": "provider",
        "providerId": args.provider_id,
        "assistantMode": "composer-assistant",
        "requestedTools": ["program_draft_upsert_parameters"],
    }
    started = time.perf_counter()
    try:
        data = _post_json(
            f"{args.composer_url}/api/assistant/chat",
            payload,
            timeout_s=args.case_timeout_s,
        )
        calls = data.get("toolCalls") or []
        allowed_tools = _active_mode_tools(args, "composer-assistant")
        completed, validation_errors = _single_completed_tool_call(
            calls,
            "program_draft_upsert_parameters",
        )
        validation_errors.extend(_out_of_mode_tool_errors(calls, allowed_tools, label="toolCalls"))
        arguments = completed.get("arguments") if completed else {}
        validation_errors.extend(
            _parameter_argument_errors(
                arguments if isinstance(arguments, dict) else {},
                expected_parameter,
                label="toolCalls[0].arguments",
            )
        )
        result = completed.get("result") if completed else {}
        result = result if isinstance(result, dict) else {}
        validation_errors.extend(
            _program_parameter_result_errors(
                result,
                expected_parameter,
                label="toolCalls[0].result",
            )
        )
        program = _get_json(f"{args.composer_url}/api/program", timeout_s=args.case_timeout_s)
        parameters_by_name = _program_parameters_by_name(program)
        validation_errors.extend(
            _program_parameter_match_errors(
                parameters_by_name.get(expected_parameter["name"]),
                expected_parameter,
                label=f"program.parameters[{expected_parameter['name']}]",
            )
        )
        validation_errors.extend(
            _unexpected_slugged_values_errors(
                list(parameters_by_name),
                [expected_parameter["name"]],
                prefix="golden_param_",
                suffix=slug,
                label="program.parameters.names",
            )
        )
        validation_errors.extend(
            _message_mentions_errors(
                data.get("message"),
                [expected_parameter["name"]],
                label="message",
            )
        )
        status = _request_status(args, request_id)
        validation_errors.extend(
            _request_status_errors(
                status,
                request_id=request_id,
                assistant_mode="composer-assistant",
                provider_id=args.provider_id,
                draft_mutated=True,
                tool_call_count=1,
                label="requestStatus",
            )
        )
        discard = _discard_parameter_by_reload(args, expected_parameter["name"])
        return {
            "ok": (
                not validation_errors
                and data.get("draftMutated") is True
                and discard.get("ok") is True
            ),
            "requestId": request_id,
            "durationS": round(time.perf_counter() - started, 3),
            "draftMutated": data.get("draftMutated"),
            "expectedParameter": expected_parameter,
            "toolCallCount": len(calls) if isinstance(calls, list) else 0,
            "validationErrors": validation_errors,
            "toolCalls": _compact_tool_calls(calls),
            "requestStatus": status,
            "discardByReload": discard,
            "message": data.get("message", "")[:240],
        }
    except Exception as exc:  # noqa: BLE001
        _load_baseline(args)
        return {"ok": False, "durationS": round(time.perf_counter() - started, 3), "error": str(exc)}


def _golden_blackboard_upsert_case(args: argparse.Namespace, profile_name: str) -> dict[str, Any]:
    slug = _slug(profile_name)
    expected_namespace = {
        "id": f"golden_ns_{slug}",
        "contract": "selection_context_v1",
        "prefix": f"/golden/{slug}",
        "description": "Assistant golden E2E namespace.",
        "fields": [{"name": "object_id", "type": "string"}],
    }
    expected_key = {
        "id": f"golden_key_{slug}",
        "type": "bool",
        "key": f"/golden/{slug}/ready",
        "description": "Assistant golden E2E ready flag.",
    }
    request_id = f"smoke-golden-blackboard-{slug}-{int(time.time() * 1000)}"
    payload = {
        "requestId": request_id,
        "message": _golden_blackboard_upsert_prompt(expected_namespace, expected_key),
        "mode": "provider",
        "providerId": args.provider_id,
        "assistantMode": "composer-assistant",
        "requestedTools": [
            "blackboard_draft_upsert_namespaces",
            "blackboard_draft_upsert_keys",
        ],
    }
    started = time.perf_counter()
    try:
        data = _post_json(
            f"{args.composer_url}/api/assistant/chat",
            payload,
            timeout_s=args.case_timeout_s,
        )
        calls = data.get("toolCalls") or []
        allowed_tools = _active_mode_tools(args, "composer-assistant")
        namespace_call, namespace_errors = _single_completed_tool_call(
            calls,
            "blackboard_draft_upsert_namespaces",
            exact_total=False,
        )
        key_call, key_errors = _single_completed_tool_call(
            calls,
            "blackboard_draft_upsert_keys",
            exact_total=False,
        )
        validation_errors = namespace_errors + key_errors
        validation_errors.extend(_out_of_mode_tool_errors(calls, allowed_tools, label="toolCalls"))
        validation_errors.extend(_exact_string_set_errors(_tool_call_names_from_response(calls), [
            "blackboard_draft_upsert_namespaces",
            "blackboard_draft_upsert_keys",
        ], label="toolCalls.names"))
        namespace_arguments = namespace_call.get("arguments") if namespace_call else {}
        key_arguments = key_call.get("arguments") if key_call else {}
        validation_errors.extend(
            _blackboard_namespace_argument_errors(
                namespace_arguments if isinstance(namespace_arguments, dict) else {},
                expected_namespace,
                label="namespaceTool.arguments",
            )
        )
        validation_errors.extend(
            _blackboard_key_argument_errors(
                key_arguments if isinstance(key_arguments, dict) else {},
                expected_key,
                label="keyTool.arguments",
            )
        )
        latest_result = key_call.get("result") if key_call else namespace_call.get("result") if namespace_call else {}
        latest_result = latest_result if isinstance(latest_result, dict) else {}
        validation_errors.extend(
            _blackboard_result_errors(
                latest_result,
                expected_namespace,
                expected_key,
                label="finalTool.result",
            )
        )
        program = _get_json(f"{args.composer_url}/api/program", timeout_s=args.case_timeout_s)
        namespaces_by_id, keys_by_id = _program_blackboard_maps(program)
        validation_errors.extend(
            _blackboard_namespace_match_errors(
                namespaces_by_id.get(expected_namespace["id"]),
                expected_namespace,
                label=f"program.blackboard.namespaces[{expected_namespace['id']}]",
            )
        )
        validation_errors.extend(
            _blackboard_key_match_errors(
                keys_by_id.get(expected_key["id"]),
                expected_key,
                label=f"program.blackboard.keys[{expected_key['id']}]",
            )
        )
        validation_errors.extend(
            _unexpected_slugged_values_errors(
                list(namespaces_by_id),
                [expected_namespace["id"]],
                prefix="golden_ns_",
                suffix=slug,
                label="program.blackboard.namespaces.ids",
            )
        )
        validation_errors.extend(
            _unexpected_slugged_values_errors(
                list(keys_by_id),
                [expected_key["id"]],
                prefix="golden_key_",
                suffix=slug,
                label="program.blackboard.keys.ids",
            )
        )
        validation_errors.extend(
            _message_mentions_errors(
                data.get("message"),
                [expected_namespace["id"], expected_key["id"]],
                label="message",
            )
        )
        status = _request_status(args, request_id)
        validation_errors.extend(
            _request_status_errors(
                status,
                request_id=request_id,
                assistant_mode="composer-assistant",
                provider_id=args.provider_id,
                draft_mutated=True,
                tool_call_count=2,
                label="requestStatus",
            )
        )
        discard = _discard_blackboard_by_reload(
            args,
            namespace_id=expected_namespace["id"],
            key_id=expected_key["id"],
        )
        return {
            "ok": (
                not validation_errors
                and data.get("draftMutated") is True
                and discard.get("ok") is True
            ),
            "requestId": request_id,
            "durationS": round(time.perf_counter() - started, 3),
            "draftMutated": data.get("draftMutated"),
            "expectedNamespace": expected_namespace,
            "expectedKey": expected_key,
            "toolCallCount": len(calls) if isinstance(calls, list) else 0,
            "validationErrors": validation_errors,
            "toolCalls": _compact_tool_calls(calls),
            "requestStatus": status,
            "discardByReload": discard,
            "message": data.get("message", "")[:240],
        }
    except Exception as exc:  # noqa: BLE001
        _load_baseline(args)
        return {"ok": False, "durationS": round(time.perf_counter() - started, 3), "error": str(exc)}


def _draft_tool_surface_case(args: argparse.Namespace, profile_name: str) -> dict[str, Any]:
    slug = _slug(profile_name)
    started = time.perf_counter()
    catalog_hash = _active_mode_catalog_hash(args, "composer-assistant")

    def call(tool_id: str, arguments: dict[str, Any]) -> dict[str, Any]:
        return _post_json(
            f"{args.composer_url}/api/assistant/bridge/tools/{tool_id}",
            {
                "requestId": f"smoke-surface-{slug}-{tool_id.replace('.', '-')}",
                "assistantMode": "composer-assistant",
                "catalogHash": catalog_hash,
                "arguments": arguments,
            },
            timeout_s=args.case_timeout_s,
        )

    try:
        discovery = {
            "catalog": call("catalog_read", {"entryIds": ["remove_collision_object"]}),
            "skills": call("skills_read", {"includeBodies": False}),
            "capabilities": call("deployment_capabilities_read", {}),
        }
        scene_object_id = f"assistant_surface_{slug}"
        scene_resource = {
            "objectId": scene_object_id,
            "shape": {"type": "box", "box_dimensions_m": [0.04, 0.05, 0.06]},
            "pose": {
                "frame_id": "universe",
                "position_m": [0.12, -0.22, 0.03],
                "orientation_rpy_deg": [0.0, 0.0, 0.0],
            },
        }
        upsert_scene = call("scene_draft_upsert_objects", {"sceneResources": [scene_resource]})
        remove_scene = call("scene_draft_remove_objects", {"objectIds": [scene_object_id]})

        temp_node_name = f"assistant_surface_remove_{slug}"
        insert_tree = call(
            "tree_draft_insert_node",
            {
                "parentName": "pick_and_place",
                "index": 0,
                "node": {
                    "id": "remove_collision_object",
                    "name": temp_node_name,
                    "params": {"object_id": scene_object_id},
                },
            },
        )
        update_tree = call(
            "tree_draft_update_node_params",
            {
                "nodeName": temp_node_name,
                "params": {"object_id": f"{scene_object_id}_renamed"},
            },
        )
        move_tree = call(
            "tree_draft_move_node",
            {"nodeName": temp_node_name, "newParentName": "pick_and_place", "index": 1},
        )
        replace_tree = call(
            "tree_draft_replace_subtree",
            {
                "nodeName": temp_node_name,
                "subtree": {
                    "id": "remove_collision_object",
                    "name": f"assistant_surface_replacement_{slug}",
                    "params": {"object_id": f"{scene_object_id}_replacement"},
                },
            },
        )
        delete_tree = call(
            "tree_draft_delete_node",
            {"nodeName": f"assistant_surface_replacement_{slug}"},
        )

        param_name = f"assistant_surface_param_{slug}"
        upsert_param = call(
            "program_draft_upsert_parameters",
            {
                "parameters": [
                    {
                        "name": param_name,
                        "type": "string",
                        "default": "graspable",
                        "description": "Assistant smoke temporary parameter.",
                    }
                ]
            },
        )
        remove_param = call("program_draft_remove_parameters", {"names": [param_name]})

        namespace_id = f"assistant_surface_ns_{slug}"
        key_id = f"assistant_surface_key_{slug}"
        upsert_namespace = call(
            "blackboard_draft_upsert_namespaces",
            {
                "namespaces": [
                    {
                        "id": namespace_id,
                        "contract": "selection_context_v1",
                        "prefix": f"/{namespace_id}",
                        "fields": [{"name": "object_id", "type": "string"}],
                    }
                ]
            },
        )
        upsert_key = call(
            "blackboard_draft_upsert_keys",
            {"keys": [{"id": key_id, "type": "bool", "key": f"/{key_id}"}]},
        )
        remove_key = call("blackboard_draft_remove_keys", {"ids": [key_id]})
        remove_namespace = call("blackboard_draft_remove_namespaces", {"ids": [namespace_id]})

        call_results = {
            "discovery": discovery,
            "upsertScene": upsert_scene,
            "removeScene": remove_scene,
            "insertTree": insert_tree,
            "updateTree": update_tree,
            "moveTree": move_tree,
            "replaceTree": replace_tree,
            "deleteTree": delete_tree,
            "upsertParameter": upsert_param,
            "removeParameter": remove_param,
            "upsertNamespace": upsert_namespace,
            "upsertKey": upsert_key,
            "removeKey": remove_key,
            "removeNamespace": remove_namespace,
        }
        return {
            "ok": all(
                isinstance(item, dict) and item.get("success") is True
                for key, item in call_results.items()
                if key != "discovery"
            )
            and all(
                isinstance(item, dict) and item.get("success") is True
                for item in discovery.values()
            ),
            "durationS": round(time.perf_counter() - started, 3),
            "checkedTools": sorted(
                [
                    "catalog_read",
                    "skills_read",
                    "deployment_capabilities_read",
                    "scene_draft_upsert_objects",
                    "scene_draft_remove_objects",
                    "tree_draft_insert_node",
                    "tree_draft_update_node_params",
                    "tree_draft_move_node",
                    "tree_draft_replace_subtree",
                    "tree_draft_delete_node",
                    "program_draft_upsert_parameters",
                    "program_draft_remove_parameters",
                    "blackboard_draft_upsert_namespaces",
                    "blackboard_draft_upsert_keys",
                    "blackboard_draft_remove_keys",
                    "blackboard_draft_remove_namespaces",
                ]
            ),
            "toolResults": {
                key: {
                    "success": value.get("success"),
                    "draftMutated": value.get("draftMutated"),
                    "toolId": value.get("toolId"),
                    "error": value.get("error"),
                }
                for key, value in call_results.items()
                if key != "discovery"
            },
        }
    except Exception as exc:  # noqa: BLE001
        _load_baseline(args)
        return {"ok": False, "durationS": round(time.perf_counter() - started, 3), "error": str(exc)}


def _cancel_proxy_case(args: argparse.Namespace, profile_name: str) -> dict[str, Any]:
    request_id = f"smoke-cancel-{_slug(profile_name)}-{int(time.time() * 1000)}"
    payload = {
        "requestId": request_id,
        "message": (
            "Call program_read exactly once, then give a concise status summary. "
            "This request may be cancelled by the operator."
        ),
        "mode": "provider",
        "providerId": args.provider_id,
        "assistantMode": "query",
        "requestedTools": ["program_read"],
    }
    result: dict[str, Any] = {}

    def _run_chat() -> None:
        try:
            result["chat"] = _post_json(
                f"{args.composer_url}/api/assistant/chat",
                payload,
                timeout_s=args.case_timeout_s,
            )
        except Exception as exc:  # noqa: BLE001 - returned in smoke payload
            result["chatError"] = str(exc)

    started = time.perf_counter()
    thread = threading.Thread(target=_run_chat, daemon=True)
    thread.start()
    observed_running = _wait_for_request_status(
        args,
        request_id,
        allowed_statuses={"running", "cancel_requested", "completed", "failed", "cancelled"},
        timeout_s=min(5.0, args.case_timeout_s),
    )
    cancel_response: dict[str, Any] | None = None
    cancel_error = ""
    if observed_running is not None and observed_running.get("status") == "running":
        try:
            cancel_response = _post_json(
                f"{args.composer_url}/api/assistant/requests/{request_id}/cancel",
                {},
                timeout_s=min(10.0, args.case_timeout_s),
            )
        except Exception as exc:  # noqa: BLE001 - returned in smoke payload
            cancel_error = str(exc)
    thread.join(timeout=args.case_timeout_s)
    final_status = _request_status(args, request_id) if observed_running is not None else None
    cancel_accepted = bool(cancel_response and cancel_response.get("cancelled") is True)
    completed_before_cancel = bool(
        observed_running
        and observed_running.get("status") in {"completed", "failed", "cancelled"}
    )
    return {
        "ok": cancel_accepted or completed_before_cancel,
        "requestId": request_id,
        "durationS": round(time.perf_counter() - started, 3),
        "observedStatus": observed_running,
        "cancelResponse": cancel_response,
        "cancelError": cancel_error,
        "finalStatus": final_status,
        "completedBeforeCancel": completed_before_cancel,
        "chatReturned": "chat" in result,
        "chatError": result.get("chatError", ""),
    }


def _composer_cancelled_error_case(args: argparse.Namespace) -> dict[str, Any]:
    request_id = f"negative-cancelled-{int(time.time() * 1000)}"
    try:
        cancel_response = _post_json(
            f"{args.bridge_url}/v1/manyforge/assistant/{request_id}/cancel",
            {},
            timeout_s=min(10.0, args.case_timeout_s),
        )
        data = _post_json(
            f"{args.composer_url}/api/assistant/chat",
            {
                "requestId": request_id,
                "message": "Call program_read exactly once.",
                "mode": "provider",
                "providerId": args.provider_id,
                "assistantMode": "query",
                "requestedTools": ["program_read"],
            },
            timeout_s=args.case_timeout_s,
        )
        status = _request_status(args, request_id)
        error = data.get("error") if isinstance(data.get("error"), dict) else {}
        ok = (
            cancel_response.get("cancelled") is True
            and error.get("code") == "cancelled"
            and status.get("status") == "cancelled"
            and (status.get("error") or {}).get("code") == "cancelled"
        )
        return {
            "ok": ok,
            "label": "Composer surfaces structured provider cancellation errors",
            "requestId": request_id,
            "cancelResponse": cancel_response,
            "chatError": error,
            "requestStatus": status,
        }
    except Exception as exc:  # noqa: BLE001
        return {
            "ok": False,
            "label": "Composer surfaces structured provider cancellation errors",
            "requestId": request_id,
            "detail": str(exc)[:500],
        }


def _discard_draft_by_reload(args: argparse.Namespace, object_id: str) -> dict[str, Any]:
    _load_baseline(args)
    program = _get_json(f"{args.composer_url}/api/program", timeout_s=args.case_timeout_s)
    scene_ids = [item.get("objectId") for item in program.get("sceneResources", [])]
    return {"ok": object_id not in scene_ids, "sceneContainsObjectAfterReload": object_id in scene_ids}


def _discard_tree_by_reload(args: argparse.Namespace, node_name: str) -> dict[str, Any]:
    _load_baseline(args)
    tree_response = _get_json(f"{args.composer_url}/api/program/tree", timeout_s=args.case_timeout_s)
    contains = _find_tree_node_by_name(tree_response.get("tree"), node_name) is not None
    return {"ok": not contains, "treeContainsNodeAfterReload": contains}


def _discard_parameter_by_reload(args: argparse.Namespace, parameter_name: str) -> dict[str, Any]:
    _load_baseline(args)
    program = _get_json(f"{args.composer_url}/api/program", timeout_s=args.case_timeout_s)
    names = [
        item.get("name")
        for item in program.get("parameters") or []
        if isinstance(item, dict)
    ]
    return {"ok": parameter_name not in names, "parameterPresentAfterReload": parameter_name in names}


def _discard_blackboard_by_reload(args: argparse.Namespace, *, namespace_id: str, key_id: str) -> dict[str, Any]:
    _load_baseline(args)
    program = _get_json(f"{args.composer_url}/api/program", timeout_s=args.case_timeout_s)
    blackboard = program.get("blackboard") if isinstance(program.get("blackboard"), dict) else {}
    namespace_ids = [
        item.get("id")
        for item in (blackboard.get("namespaces") or [])
        if isinstance(item, dict)
    ]
    key_ids = [
        item.get("id")
        for item in (blackboard.get("keys") or [])
        if isinstance(item, dict)
    ]
    namespace_present = namespace_id in namespace_ids
    key_present = key_id in key_ids
    return {
        "ok": not namespace_present and not key_present,
        "namespacePresentAfterReload": namespace_present,
        "keyPresentAfterReload": key_present,
    }


def _load_baseline(args: argparse.Namespace) -> None:
    _post_json(
        f"{args.composer_url}/api/deployment/load",
        {"path": args.deployment_path},
        timeout_s=args.case_timeout_s,
    )
    _post_json(
        f"{args.composer_url}/api/program/load",
        {
            "path": args.program_path,
            "deploymentPath": args.deployment_path,
            "forceDiscardOverrides": True,
        },
        timeout_s=args.case_timeout_s,
    )


def _request_status(args: argparse.Namespace, request_id: str) -> dict[str, Any]:
    return _get_json(
        f"{args.composer_url}/api/assistant/requests/{request_id}",
        timeout_s=args.case_timeout_s,
    )


def _hmi_status(infra_status: dict[str, Any]) -> dict[str, Any]:
    components = infra_status.get("components") if isinstance(infra_status.get("components"), dict) else {}
    hmi = components.get("hmi") if isinstance(components.get("hmi"), dict) else {}
    return {
        "status": hmi.get("status"),
        "port": hmi.get("port"),
    }


def _scene_diagnostic_for_object(
    args: argparse.Namespace,
    object_id: str,
    hmi_status: dict[str, Any],
) -> dict[str, Any]:
    try:
        diagnostics = _get_json(
            f"{args.composer_url}/api/scene/diagnostics",
            timeout_s=args.case_timeout_s,
        )
    except Exception as exc:  # noqa: BLE001 - diagnostics are supplemental unless required
        return {
            "checked": False,
            "present": False,
            "hmiAvailable": hmi_status.get("status") == "healthy",
            "error": str(exc)[:240],
        }
    objects = diagnostics.get("objects") if isinstance(diagnostics.get("objects"), list) else []
    match = next(
        (
            item
            for item in objects
            if isinstance(item, dict)
            and (item.get("objectId") or item.get("object_id")) == object_id
        ),
        None,
    )
    summary = diagnostics.get("summary") if isinstance(diagnostics.get("summary"), dict) else {}
    return {
        "checked": True,
        "present": match is not None,
        "hmiAvailable": bool(summary.get("hmiAvailable")),
        "match": bool(match.get("match")) if isinstance(match, dict) else False,
        "mismatchReason": (
            match.get("mismatchReason") or match.get("mismatch_reason")
            if isinstance(match, dict)
            else None
        ),
        "summary": summary,
    }


def _wait_for_request_status(
    args: argparse.Namespace,
    request_id: str,
    *,
    allowed_statuses: set[str],
    timeout_s: float,
) -> dict[str, Any] | None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            status = _request_status(args, request_id)
            if status.get("status") in allowed_statuses:
                return status
        except Exception:
            pass
        time.sleep(0.1)
    return None


def _active_mode_tools(args: argparse.Namespace, mode_id: str) -> list[str]:
    try:
        deployment = _get_json(
            f"{args.composer_url}/api/deployment/full",
            timeout_s=args.case_timeout_s,
        )
        modes = deployment.get("assistantModes") or {}
        mode = modes.get(mode_id) or {}
        catalog = mode.get("catalog") or {}
        tools = catalog.get("tools") or []
    except Exception:
        if mode_id != "composer-assistant":
            raise
        # Compatibility fallback for already-running Composer instances that
        # predate the response-model update for composer_draft_mutating tools.
        tools = [
            "catalog_read",
            "skills_read",
            "deployment_capabilities_read",
            "inspect_isaac_scene",
            "propose_scene_objects",
            "propose_scene_object_nodes",
            "scene_draft_add_object",
            "scene_draft_update_object",
            "scene_draft_upsert_objects",
            "scene_draft_remove_objects",
            "tree_draft_insert_node",
            "tree_draft_update_node_params",
            "tree_draft_delete_node",
            "tree_draft_move_node",
            "tree_draft_replace_subtree",
            "program_draft_upsert_parameters",
            "program_draft_remove_parameters",
            "blackboard_draft_upsert_namespaces",
            "blackboard_draft_remove_namespaces",
            "blackboard_draft_upsert_keys",
            "blackboard_draft_remove_keys",
        ]
    if not isinstance(tools, list) or not all(isinstance(tool, str) for tool in tools):
        raise SmokeFailure(f"assistantModes.{mode_id}.catalog.tools is not a list of strings")
    return tools


def _active_mode_catalog_hash(args: argparse.Namespace, mode_id: str) -> str:
    mode = _get_json(
        f"{args.composer_url}/api/assistant/modes/{urllib.parse.quote(mode_id)}",
        timeout_s=args.case_timeout_s,
    )
    catalog_hash = mode.get("catalogHash")
    if not isinstance(catalog_hash, str) or not catalog_hash.strip():
        raise SmokeFailure(f"/api/assistant/modes/{mode_id} did not return catalogHash")
    return catalog_hash.strip()


def _check_health(composer_url: str) -> None:
    _get_json(f"{composer_url}/api/infra/status", timeout_s=5.0)


def _resolve_model(upstream_base_url: str) -> str | None:
    data = _get_json(f"{upstream_base_url.rstrip('/')}/models", timeout_s=10.0)
    models = data.get("data") or []
    if isinstance(models, list) and models:
        model_id = models[0].get("id")
        if isinstance(model_id, str) and model_id.strip():
            return model_id.strip()
    return None


def _get_json(url: str, *, timeout_s: float) -> dict[str, Any]:
    request = urllib.request.Request(url, headers={"Accept": "application/json"}, method="GET")
    return _open_json(request, timeout_s=timeout_s)


def _post_json(url: str, payload: dict[str, Any], *, timeout_s: float) -> dict[str, Any]:
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", "Accept": "application/json"},
        method="POST",
    )
    return _open_json(request, timeout_s=timeout_s)


def _open_json(request: urllib.request.Request, *, timeout_s: float) -> dict[str, Any]:
    try:
        with urllib.request.urlopen(request, timeout=timeout_s) as response:
            raw = response.read().decode("utf-8", errors="replace")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise SmokeFailure(f"HTTP {exc.code} from {request.full_url}: {detail[:500]}") from exc
    except urllib.error.URLError as exc:
        raise SmokeFailure(f"request failed for {request.full_url}: {exc}") from exc
    except TimeoutError as exc:
        raise SmokeFailure(f"request timed out after {timeout_s}s for {request.full_url}") from exc
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise SmokeFailure(f"non-JSON response from {request.full_url}: {raw[:200]}") from exc
    if not isinstance(data, dict):
        raise SmokeFailure(f"JSON response from {request.full_url} must be an object")
    return data


def _expect_http_error(
    label: str,
    fn: Any,
    url: str,
    payload: dict[str, Any],
    *,
    timeout_s: float,
    expected_status: int,
) -> dict[str, Any]:
    try:
        fn(url, payload, timeout_s=timeout_s)
    except SmokeFailure as exc:
        ok = f"HTTP {expected_status}" in str(exc)
        return {"ok": ok, "label": label, "detail": str(exc)[:500]}
    return {
        "ok": False,
        "label": label,
        "detail": f"expected HTTP {expected_status}, request succeeded",
    }


def _add_object_prompt(object_id: str) -> str:
    return (
        "Call scene_draft_add_object exactly once to add a box scene resource. "
        f"Use sceneResource.objectId {object_id}, shape.type box, "
        "shape.box_dimensions_m [0.05, 0.05, 0.05], pose.frame_id world, "
        "pose.position_m [0.55, 0.0, 0.025]. Then briefly report the edit."
    )


def _golden_scene_resources(slug: str) -> list[dict[str, Any]]:
    return [
        {
            "objectId": f"golden_box_{slug}",
            "shape": {"type": "box", "box_dimensions_m": [0.08, 0.04, 0.03]},
            "pose": {
                "frame_id": "universe",
                "position_m": [0.45, -0.10, 0.015],
                "orientation_rpy_deg": [0.0, 0.0, 0.0],
            },
        },
        {
            "objectId": f"golden_sphere_{slug}",
            "shape": {"type": "sphere", "sphere_radius_m": 0.025},
            "pose": {
                "frame_id": "universe",
                "position_m": [0.55, 0.12, 0.025],
                "orientation_rpy_deg": [0.0, 0.0, 0.0],
            },
        },
    ]


def _golden_scene_prompt(resources: list[dict[str, Any]]) -> str:
    return (
        "Call scene_draft_upsert_objects exactly once. "
        "Use arguments.sceneResources exactly equal to this JSON array, with no extra objects: "
        f"{json.dumps(resources, separators=(',', ':'))}. "
        "Then briefly report the two object IDs."
    )


def _golden_tree_insert_prompt(arguments: dict[str, Any]) -> str:
    return (
        "Call tree_draft_insert_node exactly once. "
        "Use arguments exactly equal to this JSON object, with no extra keys: "
        f"{json.dumps(arguments, separators=(',', ':'))}. "
        "Then briefly report the inserted node name."
    )


def _golden_parameter_upsert_prompt(parameter: dict[str, Any]) -> str:
    arguments = {"parameters": [parameter]}
    return (
        "Call program_draft_upsert_parameters exactly once. "
        "Use arguments exactly equal to this JSON object, with no extra keys: "
        f"{json.dumps(arguments, separators=(',', ':'))}. "
        "Then briefly report the parameter name."
    )


def _golden_blackboard_upsert_prompt(namespace: dict[str, Any], key: dict[str, Any]) -> str:
    namespace_arguments = {"namespaces": [namespace]}
    key_arguments = {"keys": [key]}
    return (
        "Call blackboard_draft_upsert_namespaces exactly once, then call "
        "blackboard_draft_upsert_keys exactly once. Use the namespace tool arguments "
        "exactly equal to this JSON object: "
        f"{json.dumps(namespace_arguments, separators=(',', ':'))}. "
        "Use the key tool arguments exactly equal to this JSON object: "
        f"{json.dumps(key_arguments, separators=(',', ':'))}. "
        "Then briefly report the namespace and key IDs."
    )


def _scene_draft_add_tool_descriptor() -> dict[str, Any]:
    return {
        "type": "function",
        "function": {
            "name": "scene_draft_add_object",
            "description": "Add a scene object to the Composer draft.",
            "parameters": {
                "type": "object",
                "required": ["sceneResource"],
                "properties": {
                    "sceneResource": {
                        "type": "object",
                        "required": ["objectId", "shape", "pose"],
                        "properties": {
                            "objectId": {"type": "string", "minLength": 1},
                            "shape": {
                                "type": "object",
                                "required": ["type"],
                                "properties": {
                                    "type": {
                                        "type": "string",
                                        "enum": ["box", "sphere", "cylinder", "mesh"],
                                    },
                                    "box_dimensions_m": {
                                        "type": "array",
                                        "items": {"type": "number"},
                                        "minItems": 3,
                                        "maxItems": 3,
                                    },
                                },
                                "additionalProperties": True,
                            },
                            "pose": {
                                "type": "object",
                                "required": ["frame_id", "position_m"],
                                "properties": {
                                    "frame_id": {"type": "string"},
                                    "position_m": {
                                        "type": "array",
                                        "items": {"type": "number"},
                                        "minItems": 3,
                                        "maxItems": 3,
                                    },
                                    "orientation_rpy_deg": {
                                        "type": "array",
                                        "items": {"type": "number"},
                                        "minItems": 3,
                                        "maxItems": 3,
                                    },
                                },
                                "additionalProperties": True,
                            },
                        },
                        "additionalProperties": False,
                    },
                    "forceDiscardOverrides": {"type": "boolean"},
                },
                "additionalProperties": False,
            },
        },
    }


def _tool_call_names(tool_calls: Any) -> list[str]:
    if not isinstance(tool_calls, list):
        return []
    names: list[str] = []
    for call in tool_calls:
        if isinstance(call, dict):
            fn = call.get("function") or {}
            name = fn.get("name")
            if isinstance(name, str):
                names.append(name)
    return names


def _compact_tool_calls(tool_calls: Any) -> list[dict[str, Any]]:
    if not isinstance(tool_calls, list):
        return []
    compact: list[dict[str, Any]] = []
    for call in tool_calls:
        if isinstance(call, dict):
            compact.append(
                {
                    "name": call.get("name"),
                    "status": call.get("status"),
                    "error": call.get("error"),
                }
            )
    return compact


def _tool_call_names_from_response(tool_calls: Any) -> list[str]:
    if not isinstance(tool_calls, list):
        return []
    return [
        call.get("name")
        for call in tool_calls
        if isinstance(call, dict) and isinstance(call.get("name"), str)
    ]


def _out_of_mode_tool_errors(tool_calls: Any, allowed_tools: list[str], *, label: str) -> list[str]:
    allowed = set(allowed_tools)
    names = _tool_call_names_from_response(tool_calls)
    outside = sorted({name for name in names if name not in allowed})
    if not outside:
        return []
    return [f"{label} contains tools outside active mode catalog: {outside}"]


def _single_completed_tool_call(
    tool_calls: Any,
    tool_name: str,
    *,
    exact_total: bool = True,
) -> tuple[dict[str, Any], list[str]]:
    if not isinstance(tool_calls, list):
        return {}, ["toolCalls must be a list"]
    matching = [
        call
        for call in tool_calls
        if isinstance(call, dict) and call.get("name") == tool_name
    ]
    completed = [
        call
        for call in matching
        if call.get("status") == "completed"
    ]
    errors: list[str] = []
    if exact_total and len(tool_calls) != 1:
        errors.append(f"expected exactly 1 tool call, got {len(tool_calls)}")
    if len(matching) != 1:
        errors.append(f"expected exactly 1 {tool_name} call, got {len(matching)}")
    if len(completed) != 1:
        errors.append(f"expected exactly 1 completed {tool_name} call, got {len(completed)}")
    return (completed[0] if len(completed) == 1 else {}), errors


def _request_status_errors(
    status: dict[str, Any],
    *,
    request_id: str,
    assistant_mode: str,
    provider_id: str,
    draft_mutated: bool,
    tool_call_count: int,
    label: str,
) -> list[str]:
    expected = {
        "status": "completed",
        "requestId": request_id,
        "assistantMode": assistant_mode,
        "providerId": provider_id,
        "draftMutated": draft_mutated,
        "toolCallCount": tool_call_count,
        "error": None,
    }
    errors: list[str] = []
    for key, expected_value in expected.items():
        if status.get(key) != expected_value:
            errors.append(f"{label}.{key} expected {expected_value!r}, got {status.get(key)!r}")
    warnings = status.get("warnings")
    if warnings not in (None, []):
        errors.append(f"{label}.warnings expected empty, got {warnings!r}")
    if not isinstance(status.get("startedAtNs"), int) or status.get("startedAtNs") <= 0:
        errors.append(f"{label}.startedAtNs must be a positive integer")
    if not isinstance(status.get("finishedAtNs"), int) or status.get("finishedAtNs") <= 0:
        errors.append(f"{label}.finishedAtNs must be a positive integer")
    if (
        isinstance(status.get("startedAtNs"), int)
        and isinstance(status.get("finishedAtNs"), int)
        and status["finishedAtNs"] < status["startedAtNs"]
    ):
        errors.append(f"{label}.finishedAtNs must be >= startedAtNs")
    return errors


def _message_mentions_errors(message: Any, expected_tokens: list[str], *, label: str) -> list[str]:
    if not isinstance(message, str) or not message.strip():
        return [f"{label} must be a non-empty string"]
    missing = [token for token in expected_tokens if token not in message]
    return [f"{label} does not mention expected token(s): {missing}"] if missing else []


def _program_scene_by_id(program: dict[str, Any]) -> dict[str, dict[str, Any]]:
    resources = program.get("sceneResources")
    if not isinstance(resources, list):
        return {}
    return {
        object_id: resource
        for resource in resources
        if isinstance(resource, dict)
        for object_id in [resource.get("objectId") or resource.get("object_id")]
        if isinstance(object_id, str)
    }


def _program_parameters_by_name(program: dict[str, Any]) -> dict[str, dict[str, Any]]:
    parameters = program.get("parameters")
    if not isinstance(parameters, list):
        return {}
    return {
        name: parameter
        for parameter in parameters
        if isinstance(parameter, dict)
        for name in [parameter.get("name")]
        if isinstance(name, str)
    }


def _program_blackboard_maps(
    program: dict[str, Any],
) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    blackboard = program.get("blackboard") if isinstance(program.get("blackboard"), dict) else {}
    namespaces = blackboard.get("namespaces")
    keys = blackboard.get("keys")
    namespaces_by_id = {
        namespace_id: namespace
        for namespace in (namespaces if isinstance(namespaces, list) else [])
        if isinstance(namespace, dict)
        for namespace_id in [namespace.get("id")]
        if isinstance(namespace_id, str)
    }
    keys_by_id = {
        key_id: key
        for key in (keys if isinstance(keys, list) else [])
        if isinstance(key, dict)
        for key_id in [key.get("id")]
        if isinstance(key_id, str)
    }
    return namespaces_by_id, keys_by_id


def _unexpected_slugged_values_errors(
    actual_values: list[str],
    expected_values: list[str],
    *,
    prefix: str,
    suffix: str,
    label: str,
) -> list[str]:
    expected = set(expected_values)
    scoped_values = {
        value
        for value in actual_values
        if isinstance(value, str) and value.startswith(prefix) and value.endswith(suffix)
    }
    unexpected = sorted(scoped_values - expected)
    return [f"{label} contains unexpected values for this golden case: {unexpected}"] if unexpected else []


def _draft_request_status_ok(status: dict[str, Any], request_id: str) -> bool:
    return (
        status.get("status") == "completed"
        and status.get("requestId") == request_id
        and status.get("draftMutated") is True
        and status.get("error") is None
    )


def _scene_object_ids(data: dict[str, Any]) -> list[str]:
    ids: list[str] = []
    for call in data.get("toolCalls") or []:
        if not isinstance(call, dict):
            continue
        result = call.get("result") or {}
        if not isinstance(result, dict):
            continue
        for resource in result.get("scene") or []:
            if isinstance(resource, dict):
                object_id = resource.get("objectId") or resource.get("object_id")
                if isinstance(object_id, str):
                    ids.append(object_id)
    return ids


def _scene_resources(data: dict[str, Any]) -> list[dict[str, Any]]:
    resources: list[dict[str, Any]] = []
    for call in data.get("toolCalls") or []:
        if not isinstance(call, dict):
            continue
        result = call.get("result") or {}
        if not isinstance(result, dict):
            continue
        for resource in result.get("scene") or []:
            if isinstance(resource, dict):
                resources.append(resource)
    return resources


def _tree_insert_argument_errors(
    actual: dict[str, Any],
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    errors: list[str] = []
    for key in ("parentName", "index"):
        if actual.get(key) != expected.get(key):
            errors.append(f"{label}.{key} expected {expected.get(key)!r}, got {actual.get(key)!r}")
    errors.extend(
        _tree_node_match_errors(
            actual.get("node"),
            expected["node"],
            label=f"{label}.node",
        )
    )
    extra = sorted(set(actual) - {"parentName", "parent_name", "index", "node", "forceDiscardOverrides"})
    if extra:
        errors.append(f"{label} contains unexpected keys: {extra}")
    return errors


def _tree_insert_result_errors(
    result: dict[str, Any],
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    errors: list[str] = []
    expected_node = expected["node"]
    expected_parent = expected["parentName"]
    expected_index = expected["index"]
    expected_values = {
        "draftMutation": "tree_node_insert",
        "nodeName": expected_node["name"],
        "parentName": expected_parent,
        "index": expected_index,
    }
    for key, value in expected_values.items():
        if result.get(key) != value:
            errors.append(f"{label}.{key} expected {value!r}, got {result.get(key)!r}")
    tree = result.get("tree")
    parent = _find_tree_node_by_name(tree, expected_parent)
    if parent is None:
        errors.append(f"{label}.tree missing parent node {expected_parent!r}")
        return errors
    children = parent.get("children") if isinstance(parent, dict) else None
    if not isinstance(children, list) or len(children) <= expected_index:
        errors.append(f"{label}.tree parent {expected_parent!r} missing child at index {expected_index}")
        return errors
    errors.extend(
        _tree_node_match_errors(
            children[expected_index],
            expected_node,
            label=f"{label}.tree[{expected_parent}].children[{expected_index}]",
        )
    )
    return errors


def _tree_node_match_errors(actual: Any, expected: dict[str, Any], *, label: str) -> list[str]:
    if not isinstance(actual, dict):
        return [f"{label} is missing or not an object"]
    errors: list[str] = []
    for key in ("id", "name"):
        if actual.get(key) != expected.get(key):
            errors.append(f"{label}.{key} expected {expected.get(key)!r}, got {actual.get(key)!r}")
    actual_params = actual.get("params") or {}
    expected_params = expected.get("params") or {}
    if actual_params != expected_params:
        errors.append(f"{label}.params expected {expected_params!r}, got {actual_params!r}")
    return errors


def _find_tree_node_by_name(node: Any, name: str) -> dict[str, Any] | None:
    if not isinstance(node, dict):
        return None
    if node.get("name") == name:
        return node
    for child in node.get("children") or []:
        found = _find_tree_node_by_name(child, name)
        if found is not None:
            return found
    return None


def _parameter_argument_errors(
    actual: dict[str, Any],
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    parameters = actual.get("parameters")
    if not isinstance(parameters, list):
        return [f"{label}.parameters must be a list"]
    errors = _single_item_errors(parameters, label=f"{label}.parameters")
    if errors:
        return errors
    return _program_parameter_match_errors(
        parameters[0],
        expected,
        label=f"{label}.parameters[0]",
    )


def _program_parameter_result_errors(
    result: dict[str, Any],
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    errors: list[str] = []
    if result.get("draftMutation") != "program_parameters_upsert":
        errors.append(
            f"{label}.draftMutation expected 'program_parameters_upsert', got {result.get('draftMutation')!r}"
        )
    names = _string_list(result.get("names"))
    errors.extend(_exact_string_set_errors(names, [expected["name"]], label=f"{label}.names"))
    parameters = result.get("parameters")
    if not isinstance(parameters, list):
        errors.append(f"{label}.parameters must be a list")
        return errors
    by_name = {
        item.get("name"): item
        for item in parameters
        if isinstance(item, dict) and isinstance(item.get("name"), str)
    }
    errors.extend(
        _program_parameter_match_errors(
            by_name.get(expected["name"]),
            expected,
            label=f"{label}.parameters[{expected['name']}]",
        )
    )
    return errors


def _program_parameter_match_errors(
    actual: Any,
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, dict):
        return [f"{label} is missing or not an object"]
    errors: list[str] = []
    for key in ("name", "type", "default", "description"):
        if actual.get(key) != expected.get(key):
            errors.append(f"{label}.{key} expected {expected.get(key)!r}, got {actual.get(key)!r}")
    return errors


def _blackboard_namespace_argument_errors(
    actual: dict[str, Any],
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    namespaces = actual.get("namespaces")
    if not isinstance(namespaces, list):
        return [f"{label}.namespaces must be a list"]
    errors = _single_item_errors(namespaces, label=f"{label}.namespaces")
    if errors:
        return errors
    return _blackboard_namespace_match_errors(
        namespaces[0],
        expected,
        label=f"{label}.namespaces[0]",
    )


def _blackboard_key_argument_errors(
    actual: dict[str, Any],
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    keys = actual.get("keys")
    if not isinstance(keys, list):
        return [f"{label}.keys must be a list"]
    errors = _single_item_errors(keys, label=f"{label}.keys")
    if errors:
        return errors
    return _blackboard_key_match_errors(keys[0], expected, label=f"{label}.keys[0]")


def _blackboard_result_errors(
    result: dict[str, Any],
    expected_namespace: dict[str, Any],
    expected_key: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    blackboard = result.get("blackboard")
    if not isinstance(blackboard, dict):
        return [f"{label}.blackboard must be an object"]
    namespaces = blackboard.get("namespaces")
    keys = blackboard.get("keys")
    if not isinstance(namespaces, list):
        namespaces = []
    if not isinstance(keys, list):
        keys = []
    namespace_by_id = {
        item.get("id"): item
        for item in namespaces
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    key_by_id = {
        item.get("id"): item
        for item in keys
        if isinstance(item, dict) and isinstance(item.get("id"), str)
    }
    errors = _blackboard_namespace_match_errors(
        namespace_by_id.get(expected_namespace["id"]),
        expected_namespace,
        label=f"{label}.blackboard.namespaces[{expected_namespace['id']}]",
    )
    errors.extend(
        _blackboard_key_match_errors(
            key_by_id.get(expected_key["id"]),
            expected_key,
            label=f"{label}.blackboard.keys[{expected_key['id']}]",
        )
    )
    return errors


def _blackboard_namespace_match_errors(
    actual: Any,
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, dict):
        return [f"{label} is missing or not an object"]
    errors: list[str] = []
    for key in ("id", "contract", "prefix", "description"):
        if actual.get(key) != expected.get(key):
            errors.append(f"{label}.{key} expected {expected.get(key)!r}, got {actual.get(key)!r}")
    actual_fields = _normalize_blackboard_fields(actual.get("fields"))
    expected_fields = _normalize_blackboard_fields(expected.get("fields"))
    if actual_fields != expected_fields:
        errors.append(f"{label}.fields expected {expected_fields!r}, got {actual_fields!r}")
    return errors


def _normalize_blackboard_fields(raw: Any) -> Any:
    if not isinstance(raw, list):
        return raw
    normalized: list[Any] = []
    for item in raw:
        if not isinstance(item, dict):
            normalized.append(item)
            continue
        field = dict(item)
        field.setdefault("description", "")
        normalized.append(field)
    return normalized


def _blackboard_key_match_errors(
    actual: Any,
    expected: dict[str, Any],
    *,
    label: str,
) -> list[str]:
    if not isinstance(actual, dict):
        return [f"{label} is missing or not an object"]
    errors: list[str] = []
    for key in ("id", "type", "key", "description"):
        if actual.get(key) != expected.get(key):
            errors.append(f"{label}.{key} expected {expected.get(key)!r}, got {actual.get(key)!r}")
    return errors


def _single_item_errors(items: list[Any], *, label: str) -> list[str]:
    if len(items) != 1:
        return [f"{label} expected exactly 1 item, got {len(items)}"]
    if not isinstance(items[0], dict):
        return [f"{label}[0] must be an object"]
    return []


def _scene_resource_matches(
    actual: dict[str, Any] | None,
    expected: dict[str, Any],
    *,
    tolerance: float = 1e-6,
) -> bool:
    return not _scene_resource_match_errors(
        actual,
        expected,
        label="sceneResource",
        tolerance=tolerance,
    )


def _tool_call_scene_resources(call: dict[str, Any]) -> list[Any]:
    arguments = call.get("arguments") if isinstance(call, dict) else {}
    if not isinstance(arguments, dict):
        return []
    resources = arguments.get("sceneResources")
    if resources is None:
        resources = arguments.get("scene_resources")
    return resources if isinstance(resources, list) else []


def _scene_resource_set_match_errors(
    actual_resources: Any,
    expected_resources: list[dict[str, Any]],
    *,
    label: str,
    require_exact: bool,
) -> list[str]:
    if not isinstance(actual_resources, list):
        return [f"{label} must be a list"]
    errors: list[str] = []
    actual_by_id: dict[str, dict[str, Any]] = {}
    duplicate_ids: set[str] = set()
    invalid_indices: list[int] = []
    for index, resource in enumerate(actual_resources):
        if not isinstance(resource, dict):
            invalid_indices.append(index)
            continue
        resource = _normalize_scene_resource_for_smoke(resource)
        object_id = resource.get("objectId") or resource.get("object_id")
        if not isinstance(object_id, str) or not object_id.strip():
            errors.append(f"{label}[{index}] is missing objectId")
            continue
        object_id = object_id.strip()
        if object_id in actual_by_id:
            duplicate_ids.add(object_id)
        actual_by_id[object_id] = resource
    for index in invalid_indices:
        errors.append(f"{label}[{index}] must be an object")
    if duplicate_ids:
        errors.append(f"{label} contains duplicate objectIds: {sorted(duplicate_ids)}")

    expected_ids = [resource["objectId"] for resource in expected_resources]
    errors.extend(
        _exact_string_set_errors(
            list(actual_by_id),
            expected_ids,
            label=f"{label}.objectIds",
        )
        if require_exact
        else [
            f"{label} missing expected objectIds: {missing}"
            for missing in [sorted(set(expected_ids) - set(actual_by_id))]
            if missing
        ]
    )
    for expected in expected_resources:
        object_id = expected["objectId"]
        actual = actual_by_id.get(object_id)
        errors.extend(
            _scene_resource_match_errors(
                actual,
                expected,
                label=f"{label}[{object_id}]",
            )
        )
    return errors


def _normalize_scene_resource_for_smoke(resource: dict[str, Any]) -> dict[str, Any]:
    """Canonicalize the LLM-facing scene-resource aliases accepted by Composer.

    Smoke validation should check semantic equivalence of accepted tool input,
    not require the model to have used only the nested canonical form.
    """

    normalized = dict(resource)
    object_id = normalized.get("objectId") or normalized.get("object_id")
    if isinstance(object_id, str):
        normalized["objectId"] = object_id

    shape = dict(normalized.get("shape") or {}) if isinstance(normalized.get("shape"), dict) else {}
    shape_type = (
        shape.get("type")
        or normalized.get("shapeType")
        or normalized.get("shape_type")
        or normalized.get("type")
    )
    if isinstance(shape_type, str):
        shape["type"] = shape_type

    box_dimensions = (
        shape.get("box_dimensions_m")
        or shape.get("dimensions_m")
        or shape.get("dimensions")
        or normalized.get("box_dimensions_m")
        or normalized.get("dimensions_m")
        or normalized.get("dimensions")
        or normalized.get("size")
        or normalized.get("size_m")
    )
    if isinstance(box_dimensions, list):
        shape["box_dimensions_m"] = box_dimensions

    radius = (
        shape.get("sphere_radius_m")
        or shape.get("radius_m")
        or shape.get("radius")
        or normalized.get("sphere_radius_m")
        or normalized.get("radius_m")
        or normalized.get("radius")
    )
    diameter = shape.get("diameter_m") or shape.get("diameter") or normalized.get("diameter_m") or normalized.get("diameter")
    if isinstance(radius, (int, float)):
        shape["sphere_radius_m"] = radius
    elif isinstance(diameter, (int, float)):
        shape["sphere_radius_m"] = float(diameter) / 2.0
    if shape:
        normalized["shape"] = shape

    pose = dict(normalized.get("pose") or {}) if isinstance(normalized.get("pose"), dict) else {}
    frame_id = pose.get("frame_id") or pose.get("frameId") or normalized.get("frame_id") or normalized.get("frameId")
    if isinstance(frame_id, str):
        pose["frame_id"] = frame_id
    position = (
        pose.get("position_m")
        or pose.get("position")
        or normalized.get("position_m")
        or normalized.get("position")
    )
    if isinstance(position, list):
        pose["position_m"] = position
    orientation_rpy = (
        pose.get("orientation_rpy_deg")
        or pose.get("orientation")
        or normalized.get("orientation_rpy_deg")
        or normalized.get("orientation")
    )
    if isinstance(orientation_rpy, list):
        pose["orientation_rpy_deg"] = orientation_rpy
    orientation_xyzw = pose.get("orientation_xyzw") or normalized.get("orientation_xyzw")
    if isinstance(orientation_xyzw, list):
        pose["orientation_xyzw"] = orientation_xyzw
    if pose:
        normalized["pose"] = pose

    return normalized


def _exact_string_set_errors(actual: list[str], expected: list[str], *, label: str) -> list[str]:
    actual_set = set(actual)
    expected_set = set(expected)
    errors: list[str] = []
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)
    if missing:
        errors.append(f"{label} missing expected values: {missing}")
    if extra:
        errors.append(f"{label} contains unexpected values: {extra}")
    return errors


def _string_list(value: Any) -> list[str]:
    if not isinstance(value, list):
        return []
    return [item for item in value if isinstance(item, str)]


def _scene_resource_match_errors(
    actual: dict[str, Any] | None,
    expected: dict[str, Any],
    *,
    label: str,
    tolerance: float = 1e-6,
) -> list[str]:
    errors: list[str] = []
    if not isinstance(actual, dict):
        return [f"{label} is missing or not an object"]
    actual_id = actual.get("objectId") or actual.get("object_id")
    expected_id = expected.get("objectId") or expected.get("object_id")
    if actual_id != expected_id:
        errors.append(f"{label}.objectId expected {expected_id!r}, got {actual_id!r}")
    shape = actual.get("shape") or {}
    expected_shape = expected.get("shape") or {}
    if shape.get("type") != expected_shape.get("type"):
        errors.append(
            f"{label}.shape.type expected {expected_shape.get('type')!r}, got {shape.get('type')!r}"
        )
    if shape.get("type") == "box":
        if not _float_lists_close(
            shape.get("box_dimensions_m"),
            expected_shape.get("box_dimensions_m"),
            tolerance=tolerance,
        ):
            errors.append(
                f"{label}.shape.box_dimensions_m expected "
                f"{expected_shape.get('box_dimensions_m')!r}, got {shape.get('box_dimensions_m')!r}"
            )
    if shape.get("type") == "sphere":
        radius = shape.get("sphere_radius_m")
        expected_radius = expected_shape.get("sphere_radius_m")
        if not isinstance(radius, (int, float)) or not isinstance(expected_radius, (int, float)):
            errors.append(
                f"{label}.shape.sphere_radius_m expected {expected_radius!r}, got {radius!r}"
            )
        elif abs(float(radius) - float(expected_radius)) > tolerance:
            errors.append(
                f"{label}.shape.sphere_radius_m expected {expected_radius!r}, got {radius!r}"
            )
    pose = actual.get("pose") or {}
    expected_pose = expected.get("pose") or {}
    if pose.get("frame_id") != expected_pose.get("frame_id"):
        errors.append(
            f"{label}.pose.frame_id expected {expected_pose.get('frame_id')!r}, "
            f"got {pose.get('frame_id')!r}"
        )
    if not _float_lists_close(
        pose.get("position_m"),
        expected_pose.get("position_m"),
        tolerance=tolerance,
    ):
        errors.append(
            f"{label}.pose.position_m expected {expected_pose.get('position_m')!r}, "
            f"got {pose.get('position_m')!r}"
        )
    for orientation_key in ("orientation_rpy_deg", "orientation_xyzw"):
        if orientation_key in expected_pose and not _float_lists_close(
            pose.get(orientation_key),
            expected_pose.get(orientation_key),
            tolerance=tolerance,
        ):
            errors.append(
                f"{label}.pose.{orientation_key} expected {expected_pose.get(orientation_key)!r}, "
                f"got {pose.get(orientation_key)!r}"
            )
    return errors


def _float_lists_close(actual: Any, expected: Any, *, tolerance: float) -> bool:
    if not isinstance(actual, list) or not isinstance(expected, list):
        return False
    if len(actual) != len(expected):
        return False
    for actual_value, expected_value in zip(actual, expected):
        if not isinstance(actual_value, (int, float)) or not isinstance(expected_value, (int, float)):
            return False
        if abs(float(actual_value) - float(expected_value)) > tolerance:
            return False
    return True


def _slug(value: str) -> str:
    slug = re.sub(r"[^a-zA-Z0-9]+", "_", value).strip("_").lower()
    return slug[:40] or "profile"


def _strict_success(results: dict[str, Any]) -> bool:
    if not results.get("ok"):
        return False
    recommended = results.get("recommendedProfile")
    profiles = results.get("profiles") or {}
    recommended_result = profiles.get(recommended) or {}
    if not (recommended_result.get("directVllm") or {}).get("ok"):
        return False
    if not (recommended_result.get("composerDraftAdd") or {}).get("ok"):
        return False
    if not (recommended_result.get("operatorComposerFlow") or {}).get("ok"):
        return False
    if not (recommended_result.get("goldenSceneUpsert") or {}).get("ok"):
        return False
    if not (recommended_result.get("goldenTreeInsert") or {}).get("ok"):
        return False
    if not (recommended_result.get("goldenParameterUpsert") or {}).get("ok"):
        return False
    if not (recommended_result.get("goldenBlackboardUpsert") or {}).get("ok"):
        return False
    if not (recommended_result.get("draftToolSurface") or {}).get("ok"):
        return False
    negative = results.get("negativeCases") or {}
    return all(
        item.get("ok")
        for key, item in negative.items()
        if isinstance(item, dict) and key not in {"auditLog", "bridgeLog"}
    )


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


if __name__ == "__main__":
    raise SystemExit(main())
