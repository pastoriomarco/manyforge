#!/usr/bin/env python3
"""Dedicated TLS front end for ManyForge MCP and health endpoints.

This proxy exposes a narrow allowlisted HTTPS route for sandbox/plugin access:

  https://host.openshell.internal:9443/api/mcp
  https://host.openshell.internal:9443/api/mcp/sse
  https://host.openshell.internal:9443/api/health
  https://host.openshell.internal:9443/api/infra/status

It exists to keep ManyForge tool traffic separate from the inference path.
The proxy enforces a dedicated bearer token and only forwards the allowlisted
endpoints to the local ManyForge backend on port 9000.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import socketserver
import ssl
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import urlsplit


class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True


_HOP_BY_HOP_HEADERS = {
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailers",
    "transfer-encoding",
    "upgrade",
}


class DedicatedRouteHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    backend_base: str = "http://127.0.0.1:9000"
    route_token: str = ""
    upstream_bearer_token: str = ""
    allowed_paths: dict[tuple[str, str], str] = {}

    def _reject(self, status: int, detail: str) -> None:
        payload = json.dumps({"detail": detail}).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(payload)

    def _check_auth(self) -> bool:
        auth = self.headers.get("Authorization", "")
        expected = f"Bearer {self.route_token}"
        if not self.route_token or auth != expected:
            self._reject(401, "Unauthorized")
            return False
        return True

    def _resolve_target(self) -> tuple[str, str] | None:
        method = self.command.upper()
        path = self.path.split("?", 1)[0]
        upstream_path = self.allowed_paths.get((method, path))
        if upstream_path is not None:
            return method, upstream_path
        self._reject(404, "Route not allowlisted")
        return None

    def _proxy(self) -> None:
        target = self._resolve_target()
        if target is None:
            return
        if not self._check_auth():
            return

        content_length = int(self.headers.get("Content-Length", "0") or "0")
        body = self.rfile.read(content_length) if content_length > 0 else None

        parsed = urlsplit(self.backend_base)
        conn = http.client.HTTPConnection(parsed.hostname, parsed.port or 80, timeout=120)
        upstream_path = target[1]
        if self.path.startswith("/api/mcp/sse") and "?" in self.path:
            upstream_path = f"{upstream_path}?{self.path.split('?', 1)[1]}"

        upstream_headers: dict[str, str] = {}
        for key, value in self.headers.items():
            if key.lower() in _HOP_BY_HOP_HEADERS:
                continue
            if key.lower() == "host":
                continue
            if key.lower() == "authorization":
                continue
            upstream_headers[key] = value
        upstream_headers["Host"] = parsed.netloc
        if self.upstream_bearer_token:
            upstream_headers["Authorization"] = f"Bearer {self.upstream_bearer_token}"
        if body is not None:
            upstream_headers["Content-Length"] = str(len(body))

        try:
            conn.request(self.command, upstream_path, body=body, headers=upstream_headers)
            resp = conn.getresponse()

            self.send_response(resp.status)
            for key, value in resp.getheaders():
                if key.lower() in _HOP_BY_HOP_HEADERS:
                    continue
                self.send_header(key, value)
            self.send_header("Connection", "close")
            self.end_headers()

            if self.command == "HEAD":
                resp.read()
                return

            while True:
                chunk = resp.read(64 * 1024)
                if not chunk:
                    break
                self.wfile.write(chunk)
        except Exception as exc:  # pragma: no cover - exercised by live transport
            self._reject(502, f"Upstream error: {exc}")
        finally:
            conn.close()
            sys.stderr.write(f"[manyforge-direct-route] {self.command} {self.path}\n")

    def do_GET(self) -> None:
        self._proxy()

    def do_HEAD(self) -> None:
        self._proxy()

    def do_POST(self) -> None:
        self._proxy()

    def do_OPTIONS(self) -> None:
        self._proxy()

    def log_message(self, fmt: str, *args) -> None:  # pragma: no cover
        return


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Dedicated TLS route for ManyForge")
    parser.add_argument("--host", default="0.0.0.0", help="Listen host")
    parser.add_argument("--port", type=int, default=9443, help="Listen port")
    parser.add_argument(
        "--backend-base",
        default="http://127.0.0.1:9000",
        help="Upstream ManyForge backend base URL",
    )
    parser.add_argument("--cert-file", required=True, help="TLS certificate PEM")
    parser.add_argument("--key-file", required=True, help="TLS private key PEM")
    parser.add_argument("--token-file", required=True, help="Bearer token file")
    parser.add_argument(
        "--upstream-bearer-token",
        default=os.environ.get("MANYFORGE_API_TOKEN", ""),
        help="Optional bearer token for the upstream backend",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    route_token = Path(args.token_file).read_text(encoding="utf-8").strip()
    if not route_token:
        raise SystemExit("Token file is empty")

    DedicatedRouteHandler.backend_base = args.backend_base
    DedicatedRouteHandler.route_token = route_token
    DedicatedRouteHandler.upstream_bearer_token = args.upstream_bearer_token
    DedicatedRouteHandler.allowed_paths = {
        ("GET", "/api/health"): "/api/health",
        ("HEAD", "/api/health"): "/api/health",
        ("GET", "/api/infra/status"): "/api/infra/status",
        ("HEAD", "/api/infra/status"): "/api/infra/status",
        ("POST", "/api/mcp"): "/api/mcp",
        ("OPTIONS", "/api/mcp"): "/api/mcp",
        ("GET", "/api/mcp/sse"): "/api/mcp/sse",
        ("OPTIONS", "/api/mcp/sse"): "/api/mcp/sse",
    }

    server = ThreadingHTTPServer((args.host, args.port), DedicatedRouteHandler)
    server.request_queue_size = 64

    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=args.cert_file, keyfile=args.key_file)
    server.socket = context.wrap_socket(server.socket, server_side=True)

    print(
        f"[manyforge-direct-route] Listening on https://{args.host}:{args.port} "
        f"-> {args.backend_base}",
        flush=True,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
