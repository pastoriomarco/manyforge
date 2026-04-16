> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Platform Requirements for Industrial ManyForge Integration

This document describes the concrete platform changes needed in OpenShell,
OpenClaw, and NemoClaw-Thor to replace the current mux-proxy workaround with a
straightforward, safe, industrially-oriented integration between a sandbox-hosted
LLM agent and a host-side ManyForge instance.

Written: 2026-04-13
Tested against: OpenShell 0.0.26, OpenClaw (embedded main agent build)

---

## Current state

The only validated transport from inside an OpenShell sandbox to a host-side
service is `inference.local:443` — a built-in gateway construct tied to the
inference provider.  To reach ManyForge, the stack currently:

1. Runs a single-threaded Python mux proxy on the host (port 8888).
2. Repoints the inference provider's `OPENAI_BASE_URL` to the mux.
3. The mux inspects each request and routes `x_manyforge`-marked traffic to
   ManyForge (port 9000), everything else to vLLM (port 8000).
4. A generated OpenClaw workspace plugin fakes tool calls through
   `inference.local/v1/completions` with a marker header, because the embedded
   agent rejects native MCP server config.

This works end-to-end but is fragile: a single-threaded proxy sharing the LLM
inference path, a generated JS plugin emulating MCP over chat completions, and
no streaming or concurrent tool-call support.

---

## What needs to change

### OpenShell

#### OS-1. Honor loaded network policies for `host.openshell.internal`

**Problem:**  The gateway loads policy entries for
`host.openshell.internal:9443` (confirmed: version 7, status Loaded).  The
CONNECT proxy at `10.200.0.1:3128` silently returns 403 for every CONNECT
request to `host.openshell.internal`, regardless of port or policy stanza
(`tls: terminate`, `access: full`, specific path rules — all ignored).

**Evidence:**

- Policy version 7 contains a `manyforge` stanza with endpoints on ports 9443,
  9000, and 8081 — all three are unreachable from within the sandbox.
- Moving the TLS front-end to `:443` and using `access: full` also fails with
  CONNECT 403 — the hostname itself is rejected, not just the port.
- Every working CONNECT target in the live policy is an external hostname
  (`api.anthropic.com`, `github.com`, `clawhub.ai`, etc.) on port 443.

**Required change:**  The proxy must evaluate the loaded network policy for
`host.openshell.internal` endpoints the same way it evaluates external
hostnames.  If a policy stanza declares an endpoint, the proxy should allow
CONNECT to it (subject to the stanza's rules, binary restrictions, and
enforcement mode).

**Impact:**  If this single change were made, the direct TLS route to ManyForge
(already implemented and tested on the host side) would work immediately from
the sandbox.  The mux proxy, the marker-header routing, and the plugin generator
would all become unnecessary.

#### OS-2. Support named service routes beyond `inference.local`

**Problem:**  `inference.local` is a gateway-internal construct: the gateway
terminates TLS, resolves the configured provider backend, and forwards traffic.
There is no operator-facing primitive to register additional named endpoints
with the same properties.

**Required change:**  Expose a general "service route" API:

```
openshell service create \
    --name manyforge.local \
    --backend host.openshell.internal:9000 \
    --tls terminate \
    --policy manyforge-policy.yaml
```

Each service route gets:
- A DNS name resolvable from inside the sandbox.
- Gateway-terminated TLS (the sandbox client uses the gateway's CA, no
  per-service certificate management needed).
- Its own network policy stanza (path rules, binary restrictions, enforcement).

`inference.local` becomes one instance of this pattern.  ManyForge becomes
another.  Future host-side services (monitoring, artifact stores, sensor
bridges) each get their own named route without competing for a single provider
URL.

#### OS-3. Document and enforce supported transport modes

**Problem:**  The policy schema accepts `tls: terminate`, `access: full`,
`protocol: rest`, and `enforcement: enforce` in any combination, but the
proxy's actual behavior is undocumented and discoverable only through live
testing.

**Required change:**  Document which combinations are supported:

| Mode | Policy fields | Proxy behavior |
|------|--------------|----------------|
| External HTTPS tunnel | `access: full`, port 443 | Raw CONNECT, client handles TLS |
| Gateway-terminated HTTPS | `tls: terminate`, port 443 | Gateway terminates TLS, enforces path rules |
| Host-local plaintext | (not currently supported) | Gateway proxies plaintext to internal host |

Reject unsupported combinations at policy load time rather than silently
accepting them and returning 403 at runtime.

---

### OpenClaw

#### OC-1. Support `mcp.servers` in the embedded main agent

**Problem:**  The embedded main agent (the agent that runs inside the OpenShell
sandbox when you do `openclaw agent`) rejects `mcp.servers` declarations in
`openclaw.json`.  The standalone agent supports them.  This forces all MCP
integrations in the sandbox to go through workspace plugins — JS modules that
emulate tool calls over HTTP, losing the native MCP protocol guarantees.

**Required change:**  The embedded agent should parse `mcp.servers` from
`openclaw.json` (or `settings.json`) and connect to declared MCP servers the
same way the standalone agent does.

```json
{
  "mcp": {
    "servers": {
      "manyforge": {
        "transport": "http",
        "url": "https://manyforge.local/api/mcp",
        "auth": { "type": "bearer", "tokenEnv": "MANYFORGE_TOKEN" }
      }
    }
  }
}
```

**Impact:**  Eliminates the workspace plugin generator, the marker-header
routing through completions endpoints, and the `x_manyforge` mux logic.  MCP
tools become native agent tools with proper JSON-RPC lifecycle, error handling,
and cancellation.

#### OC-2. Support HTTP/SSE MCP transport, not just stdio

**Problem:**  Even if OC-1 is resolved, the current MCP transport is
stdio-only: the agent spawns a subprocess and communicates over stdin/stdout.
Inside a sandbox, the MCP server is on the network (host-side), not a local
process.

**Required change:**  Support `"transport": "http"` and `"transport": "sse"` in
MCP server declarations, per the MCP specification's Streamable HTTP transport.
The agent connects to a URL, sends JSON-RPC over HTTP POST, and optionally
receives server-initiated messages over SSE.

**Impact:**  Combined with OC-1 and OS-1 (or OS-2), the full integration
becomes a config file — no generated code, no proxy, no bridge scripts.

---

### NemoClaw-Thor

#### NT-1. Separate service routing from inference provider management

**Problem:**  `openshell provider` / `openshell inference` is the only
mechanism for configuring sandbox-to-host connectivity.  Every host-side service
(vLLM, ManyForge, future sensor bridges) competes for one provider URL, leading
to the mux proxy pattern.

**Required change:**  If OpenShell adds named service routes (OS-2),
NemoClaw-Thor should manage them independently:

- `THOR_LOCAL_PROVIDER_NAME` for the LLM backend (unchanged).
- `THOR_TOOL_SERVICES` map for non-inference services:

```bash
# config.env
THOR_TOOL_SERVICES="manyforge=manyforge.local:443,sensor_bridge=sensors.local:443"
```

`configure-local-provider.sh` manages the inference provider.  A new
`configure-services.sh` (or a flag on the existing script) manages tool service
routes.  The mux proxy becomes unnecessary because each service has its own
route.

---

## Priority

| Priority | ID | Change | Unlocks |
|----------|----|--------|---------|
| **Critical** | OS-1 | Proxy honors `host.openshell.internal` policies | Direct TLS route works immediately |
| **Critical** | OC-1 | Embedded agent accepts `mcp.servers` | Native MCP without plugin hack |
| High | OC-2 | HTTP/SSE MCP transport | Network MCP servers (not just stdio) |
| High | OS-2 | Named service routes | Clean multi-service sandbox topology |
| Medium | OS-3 | Document transport modes | Operators stop guessing what works |
| Medium | NT-1 | Separate service vs inference config | Clean NemoClaw-Thor service management |

**OS-1 + OC-1** together eliminate all current workarounds.  The entire
integration becomes a policy stanza and a JSON config block.

---

## Upstream status

Searched 2026-04-13.  Repos: NVIDIA/OpenShell, NVIDIA/NemoClaw, openclaw/openclaw,
anthropics/claude-code, modelcontextprotocol/specification.

### OS-1. Proxy does not honor `host.openshell.internal` policies

Known.  Multiple related issues confirm the gap:

- **NVIDIA/OpenShell#814** (OPEN) — `allowed_ips` entries for link-local/loopback
  are silently accepted but never enforced.  The OPA policy path passes but TCP
  connections fail.
- **NVIDIA/OpenShell#807** (OPEN) — Forward-proxy 403 responses have no body,
  making it impossible to distinguish a policy denial from a connection failure.
- **NVIDIA/OpenShell#778** (CLOSED) — `host.docker.internal` unreachable on
  Linux; proxy has no route to Docker bridge even with `allowed_ips`.
- **NVIDIA/OpenShell#59** (CLOSED) — Original feat to allow sandbox egress to
  private IP space.  Implemented, but residual issues remain per #814.
- **NVIDIA/OpenShell#155** (CLOSED) — Added plain HTTP forward proxy for private
  IP endpoints (CONNECT-only before this).
- **NVIDIA/NemoClaw#1786** (OPEN) — Active user report: sandbox cannot reach
  local Ollama at `host.openshell.internal` on DGX Spark.

**Known workaround:** Use explicit IP addresses in `allowed_ips` policy and
ensure the proxy container has a route to the target.  Does not help when the
proxy itself lacks a route (our case — the gateway container cannot reach
`host.openshell.internal` on non-443 ports).

### OS-2. Named service routes beyond `inference.local`

Known feature request, no implementation:

- **NVIDIA/OpenShell#776** (OPEN) — Proposes multiple inference routes
  (`inference1.local`, `inference2.local`).  Team is "looking into provider v2."
- **NVIDIA/OpenShell#133** (CLOSED) — Original issue that created the
  `inference.local` singleton.  Design deliberately removed dynamic routing.

No workaround beyond the mux-proxy pattern we already use.

### OC-1. Embedded agent `mcp.servers` support

Partially resolved upstream but broken in practice:

- **openclaw/openclaw#43509** (CLOSED) — Original feature request for
  `mcp.servers` in `openclaw.json`.  Implemented for the standalone agent.
- **openclaw/openclaw#63394** (OPEN) — `mcp.servers.<name>.env` field is
  silently dropped at spawn time; only gateway env vars are inherited.
- **openclaw/openclaw#65525** (OPEN) — MCP stdio servers don't inherit
  `NODE_OPTIONS` or proxy env vars, breaking sandbox environments.

**Known workaround:** Set env vars in the gateway service's systemd unit or
shell environment before launching OpenClaw.  Does not help for the embedded
agent build that rejects the config entirely.

### OC-2. HTTP/SSE MCP transport

Known, actively broken with many open bugs:

In `anthropics/claude-code`:
- **#39039** (OPEN) — `claude mcp add --transport http` writes `serverUrl`
  instead of `url`, silently breaking all user-level MCPs.
- **#32708** (OPEN) — `streamable-http` transport type ignored, falls back to
  legacy SSE.
- **#33288** (OPEN) — CLI sends GET instead of POST for Streamable HTTP init.
- **#38631** (OPEN) — SSE reconnection regressed in 2.1.83.
- **#44415** (OPEN) — ~20% intermittent timeout on tool calls taking 300-500ms.
- **#39790** (OPEN) — HTTP server connection fails in v2.1.84+ with stateless
  transport.
- **#36257** (OPEN) — Missing `Accept` header breaks some HTTP MCP servers.

In `openclaw/openclaw`:
- **#29053** (OPEN) — Native support for connecting to external MCP servers
  (both stdio and HTTP).
- **#65590** (OPEN) — Streamable HTTP transport not forwarding custom headers.

**Known workaround:** Use `--transport sse` (legacy SSE) instead of
`streamable-http`.  Manually fix config to use `"url"` instead of `"serverUrl"`
if using `claude mcp add`.

### MCP in sandboxed environments (cross-cutting)

Active discussion, no implementation:

- **NVIDIA/NemoClaw#566** (OPEN) — Proposal for `nemoclaw <sandbox> mcp
  add|remove|list` to bridge host-side stdio MCP servers into sandbox (API keys
  stay on host).  Not implemented.
- **NVIDIA/NemoClaw#158** (OPEN) — MCP endpoint allowlisting for sandboxed
  agent coordination.
- **anthropics/claude-code#40769** (OPEN) — Cloud scheduled tasks blocked by
  sandbox TLS proxy (gRPC/HTTP2).

No workaround exists for running MCP tools from within OpenShell sandboxes
without copying credentials into the sandbox.

---

## Workarounds in use

| Workaround | Compensates for | Limitations |
|------------|----------------|-------------|
| Mux proxy on `:8888` | OS-1 (no direct route) | Single-threaded, no streaming, shares inference path |
| Workspace plugin generator | OC-1 (no `mcp.servers`) | Emulates MCP over completions API, no cancellation |
| `x_manyforge` header routing | OS-2 (no named routes) | Fragile convention, breaks if other middleware strips headers |
| Self-signed TLS front-end | OS-1 + OS-3 (no internal HTTPS) | Requires CA distribution, tested but blocked by proxy |
| `inference.local` repoint | OS-2 (single route) | Takes over the LLM inference path for tool calls |
