#!/usr/bin/env python3
"""Legacy reverse proxy that multiplexes vLLM and ManyForge through one port.

This is not the current Composer-assistant transport. The supported assistant
path is one ASSISTANT_PROVIDER-selected bridge per Composer process
(`openclaw` on :8200 or direct/nemoclaw on :8100). Keep this script only for
historical mux/plugin reproduction and diagnostics.

OpenShell 0.0.26's sandbox proxy only allows outbound traffic through the
`inference.local:443` TLS route.  The proxy enforces L7 path rules — only
standard OpenAI API paths are permitted:

    GET  /v1/models/**             ← allowed
    POST /v1/chat/completions      ← allowed
    POST /v1/completions           ← allowed

This proxy sits between the OpenShell inference route and both backends,
routing based on path and body content:

    GET  /v1/models/manyforge/**   → ManyForge (strip prefix)
    POST /v1/completions           → ManyForge if model=="manyforge-mcp"
                                     (body forwarded as-is to /api/mcp)
    everything else                → vLLM (pass-through)

Usage:
    python3 manyforge-mux-proxy.py [--port 8888]

From the sandbox:
    # ManyForge health (GET via /v1/models/manyforge/*)
    curl https://inference.local/v1/models/manyforge/api/health

    # ManyForge MCP (POST via /v1/completions with model=manyforge-mcp)
    curl -X POST https://inference.local/v1/completions \\
      -H 'Content-Type: application/json' \\
      -d '{"model":"manyforge-mcp","jsonrpc":"2.0","method":"tools/list","id":1}'
"""

import json
import socketserver
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError
import threading
import argparse


class ThreadingHTTPServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True

# 2026-05-09: env-var overrides allow inserting an interception proxy
# (e.g. vllm-logging-proxy.py running with mutation enabled) between the
# mux and the real vLLM backend. Defaults preserve the original behavior.
import os as _os
VLLM_BACKEND = _os.environ.get("MUX_VLLM_BACKEND", "http://127.0.0.1:8000")
MANYFORGE_BACKEND = _os.environ.get("MUX_MANYFORGE_BACKEND", "http://127.0.0.1:9000")

MANYFORGE_GET_PREFIX = "/v1/models/manyforge"
MANYFORGE_DIRECT_PREFIX = "/manyforge"
MANYFORGE_MCP_MODEL = "manyforge-mcp"


class MuxHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _route_get(self):
        """Route GET/HEAD requests based on path prefix."""
        if self.path.startswith(MANYFORGE_GET_PREFIX):
            backend_path = self.path[len(MANYFORGE_GET_PREFIX):] or "/"
            return MANYFORGE_BACKEND, backend_path
        elif self.path.startswith(MANYFORGE_DIRECT_PREFIX):
            backend_path = self.path[len(MANYFORGE_DIRECT_PREFIX):] or "/"
            return MANYFORGE_BACKEND, backend_path
        return VLLM_BACKEND, self.path

    def _route_post(self, body: bytes | None):
        """Route POST requests to ManyForge or vLLM.

        Detection: the OpenShell proxy rewrites the ``model`` field, so we
        cannot use it for routing.  Instead we look for a ``jsonrpc`` key in
        the body (present in all MCP JSON-RPC messages, never in OpenAI
        requests) combined with a custom ``x_manyforge`` marker or an
        ``X-ManyForge-Target`` header.

        Trigger conditions (any path under /v1/):
          1. Body contains ``"jsonrpc"`` AND ``"x_manyforge": true``
          2. OR header ``X-ManyForge-Target: mcp`` is present

        When triggered, the body is forwarded as-is to /api/mcp with the
        ``model`` and ``x_manyforge`` keys stripped.
        """
        is_manyforge = False
        mcp_body = body

        # Check header first (the proxy may or may not preserve custom headers)
        if self.headers.get("X-ManyForge-Target") == "mcp":
            is_manyforge = True

        # Check body for jsonrpc + x_manyforge marker
        if not is_manyforge and body and self.path.startswith("/v1/"):
            try:
                payload = json.loads(body)
                if isinstance(payload, dict) and "jsonrpc" in payload:
                    if payload.get("x_manyforge"):
                        is_manyforge = True
                    # Also detect by absence of standard OpenAI fields
                    elif "prompt" not in payload and "messages" not in payload:
                        is_manyforge = True
            except json.JSONDecodeError:
                pass

        if is_manyforge and body:
            try:
                payload = json.loads(body)
                # Strip routing markers before forwarding
                mcp_body = json.dumps(
                    {k: v for k, v in payload.items() if k not in ("model", "x_manyforge")}
                ).encode()
            except json.JSONDecodeError:
                mcp_body = body
            return MANYFORGE_BACKEND, "/api/mcp", mcp_body

        # Also support direct /manyforge prefix for host-side testing
        if self.path.startswith(MANYFORGE_DIRECT_PREFIX):
            backend_path = self.path[len(MANYFORGE_DIRECT_PREFIX):] or "/"
            return MANYFORGE_BACKEND, backend_path, body
        return VLLM_BACKEND, self.path, body

    def _proxy(self):
        # Read request body
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length) if content_length > 0 else None

        if self.command in ("GET", "HEAD", "OPTIONS"):
            backend, path = self._route_get()
        else:
            backend, path, body = self._route_post(body)

        url = f"{backend}{path}"

        # Build upstream request
        req = Request(url, data=body, method=self.command)
        skip_headers = {
            "host", "connection", "proxy-connection",
            "keep-alive", "transfer-encoding", "upgrade",
        }
        for key, val in self.headers.items():
            if key.lower() not in skip_headers:
                req.add_header(key, val)
        req.add_header("Host", backend.split("//")[1])

        # Update content-length if body was modified
        if body is not None:
            req.remove_header("Content-length")
            req.add_header("Content-Length", str(len(body)))

        target = "manyforge" if backend == MANYFORGE_BACKEND else "vllm"

        try:
            resp = urlopen(req, timeout=120)
            resp_body = resp.read()

            self.send_response(resp.status)
            for key, val in resp.getheaders():
                if key.lower() not in ("transfer-encoding", "connection"):
                    self.send_header(key, val)
            self.send_header("Content-Length", str(len(resp_body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(resp_body)
        except HTTPError as e:
            resp_body = e.read()
            self.send_response(e.code)
            for key, val in e.headers.items():
                if key.lower() not in ("transfer-encoding", "connection"):
                    self.send_header(key, val)
            self.send_header("Content-Length", str(len(resp_body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(resp_body)
        except URLError as e:
            error_msg = json.dumps({"error": str(e)}).encode()
            self.send_response(502)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(error_msg)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(error_msg)

        sys.stderr.write(f"[mux-proxy] [{target}] {self.command} {path}\n")

    def do_GET(self):
        self._proxy()

    def do_POST(self):
        self._proxy()

    def do_PUT(self):
        self._proxy()

    def do_DELETE(self):
        self._proxy()

    def do_PATCH(self):
        self._proxy()

    def do_OPTIONS(self):
        self._proxy()

    def do_HEAD(self):
        self._proxy()

    def log_message(self, format, *args):
        pass  # Suppress default logging; we log in _proxy()


def main():
    parser = argparse.ArgumentParser(description="ManyForge/vLLM multiplexing proxy")
    parser.add_argument("--port", type=int, default=8888, help="Listen port (default: 8888)")
    parser.add_argument("--host", default="0.0.0.0", help="Listen address (default: 0.0.0.0)")
    args = parser.parse_args()

    server = ThreadingHTTPServer((args.host, args.port), MuxHandler)
    server.request_queue_size = 32
    print(f"[mux-proxy] Listening on {args.host}:{args.port}", flush=True)
    print(f"[mux-proxy] Routes:", flush=True)
    print(f"[mux-proxy]   GET  /v1/models/manyforge/** → {MANYFORGE_BACKEND} (strip prefix)", flush=True)
    print(f"[mux-proxy]   POST /v1/completions model={MANYFORGE_MCP_MODEL} → {MANYFORGE_BACKEND}/api/mcp", flush=True)
    print(f"[mux-proxy]   *    /* → {VLLM_BACKEND} (pass-through)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.shutdown()


if __name__ == "__main__":
    main()
