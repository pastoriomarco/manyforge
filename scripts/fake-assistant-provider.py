#!/usr/bin/env python3
"""HTTP fixture for the ManyForge live assistant provider contract.

The fixture intentionally implements the external NemoClaw/OpenClaw side of
``manyforge.assistant.provider_request.v0`` with deterministic responses.  It
is useful for exercising the real HTTP adapter without running model inference.
"""

from __future__ import annotations

import argparse
import json
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Any
from urllib.parse import parse_qs, urlparse


REQUEST_VERSION = "manyforge.assistant.provider_request.v0"


class FakeAssistantProviderHandler(BaseHTTPRequestHandler):
    server_version = "ManyForgeFakeAssistantProvider/0.1"

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            self._send_json({"ok": True})
            return
        self._send_json({"detail": "not found"}, status=404)

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path not in {"/assistant", "/v1/manyforge/assistant"}:
            self._send_json({"detail": "not found"}, status=404)
            return

        query = parse_qs(parsed.query)
        mode = _query_value(query, "mode", "valid")
        delay_s = float(_query_value(query, "delay_s", "0") or "0")
        if delay_s > 0:
            time.sleep(delay_s)

        if mode == "http-error":
            self._send_json({"detail": "fixture provider forced HTTP error"}, status=503)
            return

        try:
            request = self._read_json_body()
        except ValueError as exc:
            self._send_json({"detail": str(exc)}, status=400)
            return

        if mode == "non-json":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"not json")
            return

        if mode == "mutating":
            self._send_json(
                {
                    "message": "Fixture provider claims it changed state.",
                    "mutated": True,
                }
            )
            return

        if mode == "malformed-proposal":
            self._send_json(
                {
                    "message": "Fixture provider returned a malformed proposal.",
                    "mutated": False,
                    "requiresReview": True,
                    "proposals": [{"id": "proposal-missing-type"}],
                }
            )
            return

        if mode == "malformed-tool-call":
            self._send_json(
                {
                    "message": "Fixture provider returned a malformed tool call.",
                    "mutated": False,
                    "requiresReview": True,
                    "toolCalls": [{"name": "inspect_isaac_scene", "status": "running"}],
                }
            )
            return

        if not _request_is_contract_shaped(request):
            self._send_json(
                {
                    "message": "Fixture provider rejected the request contract.",
                    "mutated": False,
                    "requiresReview": True,
                    "warnings": ["request did not match manyforge.assistant.provider_request.v0"],
                    "toolCalls": [
                        {
                            "name": "validate_provider_request",
                            "status": "failed",
                            "arguments": {},
                            "error": "invalid request contract",
                        }
                    ],
                },
                status=400,
            )
            return

        self._send_json(_valid_response(request))

    def log_message(self, format: str, *args: Any) -> None:
        if getattr(self.server, "quiet", False):
            return
        super().log_message(format, *args)

    def _read_json_body(self) -> dict[str, Any]:
        raw_length = self.headers.get("Content-Length", "0")
        try:
            length = int(raw_length)
        except ValueError as exc:
            raise ValueError("invalid Content-Length") from exc
        try:
            data = json.loads(self.rfile.read(length).decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise ValueError("request body must be JSON") from exc
        if not isinstance(data, dict):
            raise ValueError("request body must be a JSON object")
        return data

    def _send_json(self, payload: dict[str, Any], *, status: int = 200) -> None:
        body = json.dumps(payload, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8100)
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    server = HTTPServer((args.host, args.port), FakeAssistantProviderHandler)
    server.quiet = args.quiet
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def _query_value(query: dict[str, list[str]], key: str, default: str) -> str:
    values = query.get(key)
    if not values:
        return default
    return values[-1]


def _request_is_contract_shaped(request: dict[str, Any]) -> bool:
    if request.get("version") != REQUEST_VERSION:
        return False
    constraints = request.get("constraints")
    runtime = request.get("runtime")
    return (
        isinstance(request.get("providerId"), str)
        and isinstance(request.get("conversationId"), str)
        and isinstance(request.get("message"), str)
        and isinstance(request.get("requestedTools"), list)
        and isinstance(request.get("context"), dict)
        and isinstance(request.get("tools"), list)
        and isinstance(runtime, dict)
        and isinstance(constraints, dict)
        and constraints.get("mutatesState") is False
        and constraints.get("requiresReview") is True
        and constraints.get("proposalStatus") == "draft"
    )


def _valid_response(request: dict[str, Any]) -> dict[str, Any]:
    source_roots = request.get("context", {}).get("sourceRoots") or ["/World/soup_can"]
    source_root = source_roots[0] if source_roots else "/World/soup_can"
    resource = {
        "objectId": "fixture_soup_can",
        "shape": {"type": "box", "box_dimensions_m": [0.075, 0.075, 0.102]},
        "pose": {
            "frame_id": "universe",
            "position_m": [0.1, -0.2, 0.05],
            "orientation_rpy_deg": [0.0, 0.0, 0.0],
        },
    }
    return {
        "message": "Fixture provider prepared a reviewable scene-resource proposal.",
        "toolCalls": [
            {
                "name": "inspect_isaac_scene",
                "status": "completed",
                "arguments": {"includeSubtrees": [source_root]},
                "result": {
                    "providerId": request["providerId"],
                    "conversationId": request["conversationId"],
                    "requestedTools": request["requestedTools"],
                    "objectCount": 1,
                },
            },
            {
                "name": "propose_scene_objects",
                "status": "completed",
                "arguments": {"sourceRoots": [source_root]},
                "result": {"proposalCount": 1, "sceneResourceCount": 1},
            },
        ],
        "proposals": [
            {
                "id": "proposal-fixture-soup-can",
                "type": "scene_resource_add",
                "title": "Add fixture soup can planning object",
                "summary": "Deterministic provider fixture proposal for contract tests.",
                "status": "applied",
                "reviewRequired": False,
                "payload": {
                    "action": "add_scene_resource",
                    "sceneResource": resource,
                    "sourceRoot": source_root,
                },
                "warnings": ["fixture approximation"],
                "source": {"kind": "fake_assistant_provider", "contractVersion": REQUEST_VERSION},
            }
        ],
        "warnings": ["fixture provider only"],
        "mutated": False,
        "requiresReview": False,
    }


if __name__ == "__main__":
    raise SystemExit(main())
