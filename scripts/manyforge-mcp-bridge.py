#!/usr/bin/env python3
"""ManyForge MCP stdio-to-HTTP bridge for OpenShell sandboxes.

Translates MCP JSON-RPC over stdio (what OpenClaw expects) into HTTP POST
calls to ManyForge through the inference.local tunnel.

This script runs INSIDE the sandbox.  OpenClaw spawns it as a local MCP
server command; it reads JSON-RPC from stdin, forwards each message to
ManyForge via the mux-proxy tunnel, and writes the response to stdout.

Configuration (env vars):
    MANYFORGE_ENDPOINT  — override the target URL
                          default: https://inference.local/v1/completions

Usage in openclaw.json mcpServers:
    {
        "mcpServers": {
            "manyforge": {
                "command": "python3",
                "args": ["/sandbox/.manyforge/manyforge-mcp-bridge.py"]
            }
        }
    }
"""

import json
import os
import ssl
import sys
import urllib.request


ENDPOINT = os.environ.get(
    "MANYFORGE_ENDPOINT",
    "https://inference.local/v1/completions",
)

# Trust the OpenShell proxy CA
_CA_BUNDLE = "/etc/openshell-tls/ca-bundle.pem"
_SSL_CTX = None


def _ssl_context():
    global _SSL_CTX
    if _SSL_CTX is None:
        _SSL_CTX = ssl.create_default_context()
        if os.path.exists(_CA_BUNDLE):
            _SSL_CTX.load_verify_locations(_CA_BUNDLE)
    return _SSL_CTX


def forward_to_manyforge(message: dict) -> dict:
    """Send a JSON-RPC message to ManyForge via the mux-proxy tunnel."""
    # Add the routing marker for the mux proxy
    payload = dict(message)
    payload["x_manyforge"] = True

    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        ENDPOINT,
        data=body,
        headers={
            "Content-Type": "application/json",
            "X-ManyForge-Target": "mcp",
        },
        method="POST",
    )

    try:
        ctx = _ssl_context()
        resp = urllib.request.urlopen(req, timeout=120, context=ctx)
        return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        error_body = e.read().decode("utf-8", errors="replace")
        return {
            "jsonrpc": "2.0",
            "error": {
                "code": -32000,
                "message": f"HTTP {e.code}: {error_body[:200]}",
            },
            "id": message.get("id"),
        }
    except Exception as e:
        return {
            "jsonrpc": "2.0",
            "error": {
                "code": -32000,
                "message": str(e),
            },
            "id": message.get("id"),
        }


def main():
    # MCP stdio transport: one JSON-RPC message per line on stdin,
    # one JSON-RPC response per line on stdout.
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            message = json.loads(line)
        except json.JSONDecodeError as e:
            # Write parse error response
            error = {
                "jsonrpc": "2.0",
                "error": {"code": -32700, "message": f"Parse error: {e}"},
                "id": None,
            }
            sys.stdout.write(json.dumps(error) + "\n")
            sys.stdout.flush()
            continue

        response = forward_to_manyforge(message)
        sys.stdout.write(json.dumps(response) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
