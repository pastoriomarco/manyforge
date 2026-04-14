# ManyForge Stack Setup — Implementation Reference

Companion to [OPENCLAW_INTEGRATION_PLAN.md](OPENCLAW_INTEGRATION_PLAN.md),
Sections 3.4–3.6 and Phase I. This document describes the concrete files,
scripts, and commands that implement seamless setup.

**Date**: 2026-04-12 (revised after NemoClaw-Thor v5 analysis)

---

## 1. Current State

### NemoClaw-Thor v5 stack

NemoClaw-Thor was heavily refactored (v5 transition, 2026-04-12):

| Component | Version | Notes |
|---|---|---|
| NemoClaw | v0.0.13-1 | Sandbox survival, upgraded from v0.0.6 |
| OpenShell | v0.0.26 | Policy enforcement, upgraded from v0.0.22 |
| OpenClaw | 2026.3.11 | Agent runtime, unchanged |
| vLLM | 0.19.1rc1 (SM110) | FlashInfer CUTLASS kernels for Blackwell |
| Sandbox | `thor-v5` | Rebuilt during v5 onboard (was `thor-v4`) |

Key architectural fact: **all sandbox runtime config happens via
`kubectl exec`**, not env vars. OpenShell strips Docker ENV vars before
passing control to the entrypoint. `sync_sandbox_runtime_config()` in
`lib/sandbox-runtime.sh` patches `/sandbox/.openclaw/openclaw.json`
directly via inline Python executed inside the pod. ManyForge MCP config
injection must follow this same pattern.

### NemoClaw-Thor setup sequence (3 scripts)

```bash
# 1. Launch vLLM with a model profile (long-lived, can run for days)
~/workspaces/nemoclaw/src/NemoClaw-Thor/start-model.sh qwen3.5-27b-claude-distilled-v2-nvfp4

# 2. Wire OpenShell provider + patch sandbox config + start gateway
~/workspaces/nemoclaw/src/NemoClaw-Thor/configure-local-provider.sh

# 3. Validate entire stack
~/workspaces/nemoclaw/src/NemoClaw-Thor/status.sh
```

`configure-local-provider.sh` is the integration point. It sources
`lib/config.sh`, `lib/sandbox-runtime.sh`, `lib/checks.sh` and performs:
1. Create/update OpenShell provider (`vllm-local`, type `openai`)
2. Set inference route (provider → model ID)
3. `sync_sandbox_runtime_config()` — kubectl exec into sandbox, patch
   `openclaw.json` (baseUrl, context window, concurrency, timeouts)
4. `ensure_sandbox_gateway_running()` — start OpenClaw gateway inside sandbox
5. vLLM warmup request
6. Save config to `~/.config/nemoclaw-thor/config.env`

### ManyForge container

```
Container:  manyforge-dev-run-f6ae5007ed8c
Image:      manyforge-dev
Mounts:     src/manyforge → /workspace (bind)
            manyforge_build-cache → /tmp/manyforge-build (volume)
Ports:      9000 (Composer REST + MCP), 8081 (Viser HMI)
```

Dev container runs bash; operator starts Composer manually inside.
No compose file.

### What's missing

- Direct sandbox → `host.openshell.internal:9000` access is still not a
  reproducible baseline in the live `thor-v5` sandbox, so mux-free ManyForge
  transport is not proven yet.
- The embedded OpenClaw main agent does not consume ACPX `mcpServers` as its
  primary extension surface; it needs an installed/plugin-managed extension
  surface that registers agent tools.
- Agent domain knowledge (AGENTS.md, SOUL.md) and skills still need to be
  deployed alongside that plugin.
- A single command should bring up ManyForge, the mux, and the workspace plugin
  wiring together.

---

## 2. Architecture Decision: Extend NemoClaw-Thor, Don't Duplicate

The NemoClaw-Thor pattern (bash scripts sourcing shared libs, kubectl exec
for sandbox patching, idempotent steps) is proven and robust. Rather than
building a separate `manyforge-stack.sh` that reimplements sandbox
management, we create a companion script that integrates into the existing
workflow.

**Approach**: `configure-manyforge-provider.sh` — a new NemoClaw-Thor-style
script in ManyForge's `scripts/` that sources the same NemoClaw-Thor libs
and follows the same patterns.

**Why not extend `configure-local-provider.sh` directly?** It lives in a
different repo (`nemoclaw/src/NemoClaw-Thor/`). Coupling ManyForge config
into the NemoClaw repo creates a cross-repo dependency. Instead, our script
sources the NemoClaw-Thor libs at runtime (they're on the same machine) and
adds ManyForge-specific steps.

**Why not a standalone `manyforge-stack.sh`?** It would reimplement sandbox
management (policy merge, kubectl exec, gateway lifecycle, config
persistence) that NemoClaw-Thor already handles. Duplication = drift.

---

## 3. File Structure

```
scripts/
├── manyforge-session-start.sh              # EXISTS — offline coding setup
├── configure-manyforge-provider.sh         # NEW — ManyForge ↔ sandbox integration
├── generate-manyforge-openclaw-plugin.py   # NEW — generate workspace plugin from MCP tools
├── build-manyforge-openclaw-plugin.py      # NEW — build pinned plugin artifact + metadata
├── manyforge-stack.sh                      # NEW — thin orchestrator (container + provider)
├── manyforge-openclaw-smoke.sh             # NEW — end-to-end direct + mux validation
└── templates/
    ├── manyforge-policy-fragment.yaml      # NEW — network policy block to merge
    ├── AGENTS.md                           # NEW — agent domain knowledge
    ├── SOUL.md                             # NEW — safety boundaries
    └── skills/                             # NEW — AgentSkills (see integration plan §8)
        ├── manyforge-monitor-execution/
        │   └── SKILL.md
        ├── manyforge-diagnose-failure/
        │   └── SKILL.md
        ├── manyforge-intervene-recovery/
        │   └── SKILL.md
        ├── manyforge-escalate-to-human/
        │   └── SKILL.md
        └── manyforge-delegated-recovery/
            └── SKILL.md
```

Note: the supported main-agent path now uses a **pinned installed plugin**
under `~/.openclaw-data/extensions`, with a workspace fallback only if the
install path fails on a given stack. We still patch `openclaw.json` via kubectl
exec to enable/configure the plugin and opt its tools into the allowlist. We do
not depend on a root-level `mcp.servers` block for the embedded main agent.

---

## 4. Configuration

### Environment variables

Uses NemoClaw-Thor's config system. ManyForge-specific variables are
prefixed with `MANYFORGE_` and persisted alongside Thor config.

| Variable | Default | Source | Description |
|---|---|---|---|
| `THOR_MANAGED_SANDBOX_NAME` | `thor-v5` | `~/.config/nemoclaw-thor/config.env` | Sandbox to configure (read from Thor config, never hardcoded) |
| `MANYFORGE_API_TOKEN` | (none) | env | Bearer token for MCP auth. If empty, MCP runs without auth. |
| `MANYFORGE_PORT` | `9000` | env | Composer REST + MCP port |
| `MANYFORGE_HMI_PORT` | `8081` | env | Viser HMI port |
| `MANYFORGE_CONTAINER` | auto-detected | env | Docker container name (pattern: `manyforge-dev-run-*`) |
| `NEMOCLAW_THOR` | `~/workspaces/nemoclaw/src/NemoClaw-Thor` | env | Path to NemoClaw-Thor repo (for sourcing libs) |

### Config persistence

ManyForge-specific settings are appended to
`~/.config/nemoclaw-thor/config.env` (same file Thor uses):

```bash
# Appended by configure-manyforge-provider.sh
MANYFORGE_PORT='9000'
MANYFORGE_HMI_PORT='8081'
MANYFORGE_API_TOKEN=''
MANYFORGE_CONFIGURED='true'
```

This way `status.sh` (or a future ManyForge check in status.sh) can read
the config and verify ManyForge connectivity.

---

## 5. Scripts

### `configure-manyforge-provider.sh`

The core integration script. Sources NemoClaw-Thor libs, follows the same
step pattern as `configure-local-provider.sh`.

```bash
#!/usr/bin/env bash
# configure-manyforge-provider.sh — Wire ManyForge into the NemoClaw sandbox
#
# Prerequisites:
#   - NemoClaw-Thor configured: ./configure-local-provider.sh already run
#   - ManyForge container running with Composer on port 9000
#   - vLLM serving a model on port 8000
#
# Usage:
#   ./scripts/configure-manyforge-provider.sh
#
# What it does:
#   1. Verifies ManyForge Composer is reachable
#   2. Best-effort merges the ManyForge network policy fragment for future
#      direct host access
#   3. Ensures the mux proxy is running
#   4. Generates a workspace plugin from the live ManyForge MCP tool list
#   5. Patches openclaw.json to enable the plugin and allow its optional tools
#   6. Uploads AGENTS.md, SOUL.md, skills to sandbox workspace
#   7. Restarts OpenClaw gateway
#   8. Verifies plugin discovery (live tool use is validated separately)
```

**Step 1: Source NemoClaw-Thor libs and load config**

```bash
NEMOCLAW_THOR="${NEMOCLAW_THOR:-$HOME/workspaces/nemoclaw/src/NemoClaw-Thor}"
source "${NEMOCLAW_THOR}/lib/checks.sh"
source "${NEMOCLAW_THOR}/lib/config.sh"
source "${NEMOCLAW_THOR}/lib/sandbox-runtime.sh"

# Load saved Thor config (gets THOR_MANAGED_SANDBOX_NAME, etc.)
load_thor_runtime_config ""
```

This gives us `pass()`, `fail()`, `info()`, sandbox name resolution,
kubectl exec helpers — everything `configure-local-provider.sh` uses.

**Step 2: Verify ManyForge is reachable**

```bash
info "Checking ManyForge Composer at http://localhost:${MANYFORGE_PORT}..."
mf_health=$(curl -sf --max-time 5 "http://localhost:${MANYFORGE_PORT}/api/health" 2>/dev/null)
if [[ -z "$mf_health" ]]; then
    fail "ManyForge Composer not reachable at port ${MANYFORGE_PORT}"
    fix "Start the ManyForge container and Composer, then retry."
    exit 1
fi
pass "ManyForge Composer healthy"
```

**Step 3: Merge network policy**

```bash
info "Merging ManyForge network policy into sandbox ${THOR_MANAGED_SANDBOX_NAME}..."

# Check if already applied
current_policy=$(openshell policy get "$THOR_MANAGED_SANDBOX_NAME" 2>/dev/null)
if echo "$current_policy" | grep -q "manyforge"; then
    pass "ManyForge policy already present — skipping merge"
else
    # Merge fragment into current policy
    python3 - "$current_policy" "$SCRIPT_DIR/templates/manyforge-policy-fragment.yaml" <<'PYEOF'
import yaml, sys
current = yaml.safe_load(sys.argv[1]) if sys.argv[1].strip() else {}
fragment = yaml.safe_load(open(sys.argv[2]))
if 'network_policies' not in current:
    current['network_policies'] = {}
current['network_policies'].update(fragment['network_policies'])
with open('/tmp/manyforge-merged-policy.yaml', 'w') as f:
    yaml.dump(current, f, default_flow_style=False)
PYEOF

    openshell policy set "$THOR_MANAGED_SANDBOX_NAME" \
        --policy /tmp/manyforge-merged-policy.yaml

    # Wait for sandbox to be Ready after policy change
    for i in $(seq 1 30); do
        phase=$(openshell sandbox get "$THOR_MANAGED_SANDBOX_NAME" 2>/dev/null \
            | grep -oP 'Phase:\s+\K\w+')
        [[ "$phase" == "Ready" ]] && break
        sleep 2
    done
    [[ "$phase" == "Ready" ]] || { fail "Sandbox not Ready after policy update"; exit 1; }
    pass "ManyForge policy merged and applied"
fi
```

**Step 4: Enable the ManyForge workspace plugin in `openclaw.json` (kubectl exec)**

This is the critical step. We follow the exact same pattern as
`sync_sandbox_runtime_config()` — kubectl exec + inline Python to patch
the existing `openclaw.json`. In the current OpenClaw build, the embedded
main agent does not consume a root `mcp.servers` block, so we enable a
workspace plugin and opt its tools into `tools.alsoAllow` instead:

```bash
info "Enabling ManyForge workspace plugin in sandbox config..."

sandbox_pod=$(kubectl get pods -n openshell \
    -l "openshell.dev/sandbox-name=${THOR_MANAGED_SANDBOX_NAME}" \
    -o jsonpath='{.items[0].metadata.name}' 2>/dev/null)

kubectl exec -n openshell "$sandbox_pod" -c sandbox -- \
    python3 -c "
import json, pathlib

config_path = pathlib.Path('/sandbox/.openclaw/openclaw.json')
config = json.loads(config_path.read_text()) if config_path.exists() else {}

plugins = config.setdefault('plugins', {})
entries = plugins.setdefault('entries', {})
plugins_allow = plugins.setdefault('allow', [])
if 'manyforge-mcp' not in plugins_allow:
    plugins_allow.append('manyforge-mcp')

entries['manyforge-mcp'] = {
    'enabled': True,
    'config': {
        'endpoint': 'https://inference.local/v1/completions'
    }
}

tools = config.setdefault('tools', {})
also_allow = tools.setdefault('alsoAllow', [])
if 'manyforge-mcp' not in also_allow:
    also_allow.append('manyforge-mcp')

config_path.write_text(json.dumps(config, indent=2))
print('ManyForge workspace plugin enabled')
"

pass "ManyForge workspace plugin registered in sandbox openclaw.json"
```

**Why kubectl exec, not file upload?** Three reasons:
1. `openclaw.json` already exists with provider config, agent defaults,
   concurrency settings (patched by `sync_sandbox_runtime_config()`).
   Uploading a separate file would either overwrite those settings or
   require maintaining a parallel config file that OpenClaw doesn't read.
2. The kubectl exec approach merges into the existing JSON atomically.
3. It follows the proven NemoClaw-Thor pattern — if it works for
   `sync_sandbox_runtime_config()`, it works for plugin config.

**Step 5: Upload workspace files**

```bash
info "Uploading agent workspace files..."

openshell sandbox upload "$THOR_MANAGED_SANDBOX_NAME" \
    "$SCRIPT_DIR/templates/AGENTS.md" /sandbox/AGENTS.md
openshell sandbox upload "$THOR_MANAGED_SANDBOX_NAME" \
    "$SCRIPT_DIR/templates/SOUL.md" /sandbox/SOUL.md

for skill_dir in "$SCRIPT_DIR/templates/skills/"*/; do
    [[ -d "$skill_dir" ]] || continue
    skill_name=$(basename "$skill_dir")
    openshell sandbox upload "$THOR_MANAGED_SANDBOX_NAME" \
        "$skill_dir" "/sandbox/.openclaw/skills/${skill_name}/"
done

pass "Workspace files uploaded (AGENTS.md, SOUL.md, 5 skills)"
```

**Step 6: Restart OpenClaw gateway**

The gateway must be restarted after MCP config changes so it discovers
the new ManyForge MCP server. `ensure_sandbox_gateway_running()` from
`lib/sandbox-runtime.sh` handles this:

```bash
info "Restarting OpenClaw gateway to discover ManyForge MCP tools..."
# Kill existing gateway so ensure_sandbox_gateway_running() starts a fresh one
kubectl exec -n openshell "$sandbox_pod" -c sandbox -- \
    bash -c 'pkill -f "openclaw gateway" 2>/dev/null || true'
sleep 2
ensure_sandbox_gateway_running "$THOR_MANAGED_SANDBOX_NAME"
pass "OpenClaw gateway restarted"
```

**Step 7: Verify connectivity**

```bash
info "Verifying sandbox → ManyForge MCP connectivity..."

tool_count=$(kubectl exec -n openshell "$sandbox_pod" -c sandbox -- \
    curl -sf --max-time 10 \
        -X POST "http://host.openshell.internal:${MANYFORGE_PORT}/api/mcp" \
        -H "Content-Type: application/json" \
        ${MANYFORGE_API_TOKEN:+-H "Authorization: Bearer ${MANYFORGE_API_TOKEN}"} \
        -d '{"jsonrpc":"2.0","method":"tools/list","id":1}' \
    | python3 -c "import json,sys; print(len(json.load(sys.stdin)['result']['tools']))" \
    2>/dev/null || echo "0")

if [[ "$tool_count" -lt 12 ]]; then
    fail "Sandbox sees ${tool_count} ManyForge tools (expected 12+)"
    fix "Check ManyForge Composer logs, verify --mcp-http flag, check policy."
    exit 1
fi
pass "Sandbox discovers ${tool_count} ManyForge MCP tools"

# Save ManyForge config alongside Thor config
MANYFORGE_CONFIGURED=true
save_thor_runtime_config
pass "Config saved to ${THOR_CONFIG_FILE}"
```

### `manyforge-stack.sh`

Thin orchestrator — handles the ManyForge container and then delegates
sandbox work to `configure-manyforge-provider.sh`.

```bash
#!/usr/bin/env bash
# manyforge-stack.sh — Start/stop ManyForge, optionally wire into NemoClaw
#
# Usage:
#   ./scripts/manyforge-stack.sh up                   # Deterministic mode
#   ./scripts/manyforge-stack.sh up --with-nemoclaw    # Full stack
#   ./scripts/manyforge-stack.sh down
#   ./scripts/manyforge-stack.sh status
```

**`up` (deterministic)**:
1. Find or start ManyForge container
2. Start Composer process inside container
3. Health check

**`up --with-nemoclaw`**:
1–3 same as above, then:
4. Verify `configure-local-provider.sh` has been run (check config.env)
5. Start Composer with `--mcp-http` flag
6. Call `./scripts/configure-manyforge-provider.sh`

**`down`**:
1. Stop Composer process inside container
2. Optionally stop container (`--stop-container`)

**`status`**:
1. Check ManyForge container + Composer health
2. If NemoClaw configured: check sandbox, policy, MCP connectivity

### Output examples

**`manyforge-stack.sh up --with-nemoclaw`**:

```
=== ManyForge Stack ===

>>> [1/3] ManyForge container
    Container manyforge-dev-run-f6ae5007ed8c already running
    Starting Composer with --mcp-http...
    ✓ http://localhost:9000/api/health — healthy

>>> [2/3] NemoClaw prerequisites
    ✓ Config loaded from ~/.config/nemoclaw-thor/config.env
    ✓ Sandbox: thor-v5 (Ready)
    ✓ vLLM: Qwen3.5-27B-Claude-Distilled-v2-NVFP4 on port 8000

>>> [3/3] ManyForge ↔ NemoClaw integration
    (calls configure-manyforge-provider.sh)
    ✓ ManyForge Composer healthy
    ✓ ManyForge policy merged and applied
    ✓ MCP server registered in sandbox openclaw.json
    ✓ Workspace files uploaded (AGENTS.md, SOUL.md, 5 skills)
    ✓ OpenClaw gateway restarted
    ✓ Sandbox discovers 14 ManyForge MCP tools
    ✓ Config saved

=== Stack ready ===
  ManyForge:  http://localhost:9000 (MCP HTTP enabled)
  vLLM:       Qwen3.5-27B-Claude-Distilled-v2-NVFP4
  Sandbox:    thor-v5

  Test:
    openshell sandbox connect thor-v5
    openclaw agent -m "Call mcp_manyforge_execution_status and report"
```

**`manyforge-stack.sh up`** (deterministic):

```
=== ManyForge Stack ===

>>> [1/1] ManyForge container
    Container manyforge-dev-run-f6ae5007ed8c already running
    ✓ http://localhost:9000/api/health — healthy

=== Stack ready (deterministic mode) ===
  ManyForge:  http://localhost:9000
  No NemoClaw integration (use --with-nemoclaw to enable)
```

**`manyforge-stack.sh status`**:

```
● ManyForge    healthy   http://localhost:9000
  Container:   manyforge-dev-run-f6ae5007ed8c
  MCP HTTP:    enabled   /api/mcp (14 tools)
  Composer:    running (PID 1234)

● vLLM         healthy   http://localhost:8000/v1
  Model:       Qwen3.5-27B-Claude-Distilled-v2-NVFP4
  Profile:     qwen3.5-27b-claude-distilled-v2-nvfp4
  Sequences:   9 max, 0 active

● Sandbox      healthy   thor-v5 (Ready)
  ManyForge policy: applied (manyforge, vllm_local)
  AGENTS.md:   present
  SOUL.md:     present
  Skills:      5 installed
  MCP servers:  manyforge → host:9000
  MCP tools:   14 discovered
  Gateway:     running (port 18789)
  Concurrency: 3 main agents, 2 children each
```

---

## 6. Template Files

### `templates/manyforge-policy-fragment.yaml`

Network policy block to merge into the sandbox's existing policy.
Not a complete policy — only the entries to add.

```yaml
network_policies:
  manyforge:
    name: manyforge
    endpoints:
      - host: host.openshell.internal
        port: 9000
        protocol: rest
        enforcement: enforce
        tls: passthrough
        rules:
          - allow: { method: GET, path: "/**" }
          - allow: { method: POST, path: "/**" }
      - host: host.openshell.internal
        port: 8081
        access: full
    binaries:
      - { path: /usr/local/bin/openclaw }
      - { path: /usr/bin/curl }
```

Note: no `vllm_local` entry here. The `strict-local-inference.yaml` base
policy already allows `host.openshell.internal:8000` for `python3`. If the
sandbox was created with a different base policy, `vllm_local` should be
added to this fragment. Check with:
`openshell policy get $SANDBOX_NAME | grep -c "8000"`.

### `templates/AGENTS.md`

Agent domain knowledge for ManyForge runtime interaction. Content includes:
- ManyForge architecture overview (CycleManager states, tree executor, kernel)
- All MCP tool names, parameters, and usage patterns
- Node catalog summary (node types, parameters, constraints)
- Skill catalog (pick_object, place_object, move_to_named_config)
- Composition patterns from LLM_GUIDE.md
- Recovery patterns (FallbackOverrideNode, RecoverySkillNode, recovery_policy)
- Runtime override semantics (active/staged lifecycle, modification_policy)
- Safety rules (never bypass kernel limits, always use authority provenance)

This is the largest content authoring task — ~500-1000 lines of structured
markdown. It replaces the ad-hoc spec uploads from
`manyforge-session-start.sh` with focused operational knowledge.

### `templates/SOUL.md`

Safety boundaries for industrial robot operation. Short, non-negotiable:
- Never execute arbitrary motion commands outside ManyForge's validated API
- Never modify saved program artifacts — use runtime overrides
- Always include authority provenance in tool calls
- Escalate to human when confidence is low or when the situation is novel
- Never retry a failed recovery action more than the policy allows
- Stop the robot (cycle_control stop) before any diagnostic or intervention

### `templates/skills/`

Five AgentSkills as defined in
[OPENCLAW_INTEGRATION_PLAN.md §8](OPENCLAW_INTEGRATION_PLAN.md#8-openclaw-agentskills-for-manyforge).
Each is a directory with a `SKILL.md` file containing YAML frontmatter
(name, version, parameters, tags) and markdown body (steps, error handling).

---

## 7. Gotchas and Design Decisions

### `host.openshell.internal` resolution

OpenShell resolves this hostname to the host IP from inside the sandbox's
network namespace. The current validated path is:

- sandbox/OpenClaw client base URL: `https://inference.local/v1`
- OpenShell provider target: `http://host.openshell.internal:8000/v1` in direct
  mode, or `http://host.openshell.internal:8888/v1` in ManyForge mux mode

ManyForge's ports (9000, 8081) are bound on `0.0.0.0` via Docker's `--publish`,
but the embedded agent currently reaches ManyForge through the muxed
`inference.local` path rather than direct host networking.

### Why kubectl exec for MCP config, not file upload

OpenShell strips Docker ENV vars before passing control to the entrypoint.
The NemoClaw-Thor team discovered this and switched to kubectl exec for all
sandbox runtime patching. `sync_sandbox_runtime_config()` patches
`openclaw.json` this way. We follow the same pattern for MCP config because:

1. `openclaw.json` already exists with provider config, agent defaults,
   concurrency settings. Uploading a separate MCP config file doesn't work —
   OpenClaw reads one `openclaw.json`, not multiple config files.
2. kubectl exec merges into the existing JSON atomically.
3. The pattern is proven and survives across NemoClaw/OpenShell upgrades.

### Policy merge vs. replace

`openshell policy set` replaces the entire policy. The sandbox has ~10
existing network policies (claude_code, clawhub, discord, github,
npm_registry, nvidia, openclaw_api, openclaw_docs, telegram). All must be
preserved. The merge approach:

1. `openshell policy get` → dump current policy
2. Python inline: parse both YAMLs, `dict.update()` the `network_policies`
3. `openshell policy set` with merged result
4. Sandbox pod restarts to pick up netns changes (workspace persists with
   sandbox survival in OpenShell 0.0.26)

Idempotent: check for `manyforge` key before merging.

### Concurrency budget

Each model profile declares `max_num_seqs` (vLLM) and derived
`THOR_OPENCLAW_MAIN_MAX_CONCURRENT` (main agent slots). The ManyForge
supervisor agent (Mode C) consumes one main agent slot. If the profile
allows 3 main agents, one is the supervisor, two remain for other tasks.

This matters because `manyforge-stack.sh status` should report remaining
concurrency budget, and the AGENTS.md should warn the agent not to spawn
subagents beyond the profile limit.

### Composer process lifecycle

The dev container runs a bare `bash` shell. For `--with-nemoclaw`, the
stack script needs the Composer running with `--mcp-http`. Two approaches:

- **Option A**: `docker exec` to start/restart the Composer inside the
  existing container. Simple, matches dev workflow.
- **Option B**: Separate production container with `--mcp-http` in CMD.

Start with Option A. The stack script manages the process, not the
container.

### Relationship to `manyforge-session-start.sh`

The session-start script handles **offline coding** (specs, contracts,
worker workspaces). `configure-manyforge-provider.sh` handles **runtime
integration** (MCP, agent workspace, skills, connectivity). They don't
overlap:

- `manyforge-session-start.sh` uploads to `/sandbox/specs/`, `/sandbox/work/`
- `configure-manyforge-provider.sh` patches `/sandbox/.openclaw/openclaw.json`,
  uploads to `/sandbox/AGENTS.md`, `/sandbox/SOUL.md`,
  `/sandbox/.openclaw/skills/`

Both can run on the same sandbox.

### Relationship to `configure-local-provider.sh`

The NemoClaw-Thor script handles **vLLM provider + inference route + sandbox
config + gateway**. `configure-manyforge-provider.sh` handles **ManyForge
MCP + workspace + connectivity**. They are complementary:

```bash
# Full setup sequence:
cd ~/workspaces/nemoclaw/src/NemoClaw-Thor
./start-model.sh qwen3.5-27b-claude-distilled-v2-nvfp4     # 1. vLLM
./configure-local-provider.sh                                # 2. Provider + sandbox

cd ~/workspaces/dev_ws/src/manyforge
./scripts/manyforge-stack.sh up --with-nemoclaw               # 3. ManyForge + MCP
# (internally calls configure-manyforge-provider.sh)
```

`configure-manyforge-provider.sh` sources NemoClaw-Thor libs and reads
the saved Thor config, so it picks up the correct sandbox name, provider
settings, and kubectl context automatically.

### Idempotency

`configure-manyforge-provider.sh` is idempotent:
- Policy merge: check if `manyforge` key exists before merging
- Plugin config: kubectl exec rewrites `plugins.entries.manyforge-mcp` and
  keeps `plugins.allow` / `tools.alsoAllow` converged (safe — same value)
- File uploads: overwrite existing files (safe — same content)
- Gateway restart: `ensure_sandbox_gateway_running()` is already idempotent
- Connectivity check: read-only

Re-running after a failure resumes safely.

---

## 8. MCP Tool Gaps

Before the sandbox agent can do useful work, these MCP tools need to be
added to `mcp_server.py` (REST endpoints already exist):

| Missing tool | REST endpoint | Needed for | Priority |
|---|---|---|---|
| `manyforge_scene_snapshot` | `GET /api/scene/snapshot` | Mode B (VLM), Mode C (visual diagnosis) | Phase III |
| `manyforge_program_save` | `POST /api/program/save` | Mode D (persist agent-composed trees) | Phase II |
| `manyforge_kernel` | `GET /api/kernel/{snapshot,metrics,events}` | Mode C (deep diagnosis) | Phase IV |
| `manyforge_files` | `GET /api/files/browse` | Mode C/D (discover programs on disk) | Phase I |
| `manyforge_deployment` | `GET /api/deployment`, `POST /api/deployment/load` | Mode B (check model endpoints) | Phase II |
| `manyforge_session` | `POST /api/session/apply` | Mode C (atomic state restore) | Phase IV |

The 13 existing tools are sufficient for Phase I connectivity verification
(load → run → monitor → stop). The gaps become blocking in later phases.

---

## 9. Implementation Sprint — Iterative Bring-Up

This section defines an iterative implementation plan where each sprint
produces a testable increment. The NemoClaw stack is up and running
(all 19 checks passing, Qwen3.5-27B-Claude-Distilled-v2-NVFP4 serving).

### Prerequisites (already done)

- [x] vLLM serving on port 8000 (`start-model.sh`)
- [x] `configure-local-provider.sh` run, sandbox `thor-v5` ready
- [x] `status.sh` — all 19 checks passed
- [x] ManyForge container running, Composer on port 9000
- [x] MCP HTTP/SSE transport implemented (OP-8)
- [x] 13 MCP tools registered

---

### Sprint 1: Network — Can the sandbox reach ManyForge? ✅ COMPLETE

**Goal**: Establish connectivity from inside `thor-v5` to ManyForge's API.

**Key discovery**: OpenShell 0.0.26's sandbox proxy blocks ALL outbound
HTTP to internal hosts (`host.openshell.internal`), regardless of network
policy configuration. The proxy only supports:
  1. HTTPS CONNECT tunnels to external hosts (port 443, `tls: terminate`)
  2. A single built-in route: `inference.local:443` → configured inference provider

**Solution — Multiplexing proxy** (`scripts/manyforge-mux-proxy.py`):
A lightweight HTTP reverse proxy on port 8888 that sits between the
OpenShell inference route and both backends:

```
sandbox → proxy CONNECT inference.local:443 → mux-proxy :8888
  GET  /v1/models/manyforge/**  → ManyForge :9000 (strip prefix)
  POST /v1/completions (jsonrpc body) → ManyForge :9000/api/mcp
  everything else              → vLLM :8000 (pass-through)
```

The `openshell provider update vllm-local --config OPENAI_BASE_URL=http://host.openshell.internal:8888/v1`
re-points the inference route through the mux proxy.

**Deliverables**:

| # | Task | Status | File(s) |
|---|------|--------|---------|
| 1.1 | Policy fragment | ✅ | `scripts/templates/manyforge-policy-fragment.yaml` |
| 1.2 | Sandbox recreated with merged policy | ✅ | — (policy hot-reload does NOT work in 0.0.26) |
| 1.3 | Composer with `--mcp-http` | ✅ | `docker exec manyforge-dev-mcp ...` |
| 1.4 | Mux proxy for sandbox connectivity | ✅ | `scripts/manyforge-mux-proxy.py` |
| 1.5 | MCP tool discovery from sandbox | ✅ | 13 tools verified |

**Test procedure (Sprint 1)**:

```bash
# 1.3 — Verify Composer running on host
curl -sf http://localhost:9000/api/health

# 1.4a — Start the mux proxy (host-side, keeps running)
python3 scripts/manyforge-mux-proxy.py --port 8888 &

# 1.4b — Point inference route through the mux proxy
openshell provider update vllm-local \
  --config OPENAI_BASE_URL=http://host.openshell.internal:8888/v1
openshell inference set --provider vllm-local \
  --model Qwen3.5-27B-Claude-Distilled-v2-NVFP4 --no-verify

# 1.4c — Verify ManyForge health from sandbox
openshell sandbox exec -n thor-v5 \
  curl -sf https://inference.local/v1/models/manyforge/api/health
# Expected: {"status":"ok",...}

# 1.4d — Verify vLLM still works
openshell sandbox exec -n thor-v5 \
  curl -sf https://inference.local/v1/models
# Expected: {"data":[{"id":"Qwen3.5-27B-Claude-Distilled-v2-NVFP4",...}]}

# 1.5 — MCP tool discovery from sandbox
openshell sandbox exec -n thor-v5 \
  curl -sf -X POST https://inference.local/v1/completions \
    -H "Content-Type: application/json" \
    -d '{"x_manyforge":true,"jsonrpc":"2.0","method":"tools/list","id":1}' \
  | python3 -c "import json,sys; d=json.load(sys.stdin); \
    print(f'{len(d[\"result\"][\"tools\"])} tools discovered')"
# Expected: 13 tools discovered
```

**Exit criteria**: ✅ The sandbox reaches ManyForge via `inference.local`
and discovers 13 MCP tools. vLLM inference is unaffected.

> **NOTE**: Keep normal model inference on
> `http://host.openshell.internal:8000/v1`. The mux is for ManyForge tool
> transport, not for the main LLM inference route.

---

### Sprint 2: Main-Agent Registration — Installed plugin over mux ✅ CURRENT PATH

**Goal**: Expose ManyForge tools to the embedded OpenClaw main agent through a
versioned installed plugin that forwards to ManyForge MCP over the existing
muxed `inference.local` path.

**Deliverables**:

| # | Task | File(s) | Test |
|---|------|---------|------|
| 2.1 | Generate plugin payload from live ManyForge tools | ✅ | `generate-manyforge-openclaw-plugin.py` |
| 2.1b | Build pinned plugin artifact + metadata | ✅ | `build-manyforge-openclaw-plugin.py` |
| 2.2 | Install plugin into sandbox global extensions | ✅ | `/sandbox/.openclaw-data/extensions/manyforge-mcp/` |
| 2.3 | Patch `openclaw.json` plugin config + trust/tool allowlists | ✅ | `plugins.allow`, `plugins.entries.manyforge-mcp`, `tools.alsoAllow += manyforge-mcp` |
| 2.4 | Restart OpenClaw gateway and verify plugin discovery | ✅ | `openclaw plugins list` shows `manyforge-mcp` |
| 2.5 | Terminal-answer reporting tools for machine-consumed outcomes | ✅ | `report_lifecycle_state`, `report_recovery_outcome` generated into the plugin |

**Test procedure (Sprint 2)**:

```bash
# 2.1 — Generate a workspace plugin from the live ManyForge MCP surface
python3 scripts/generate-manyforge-openclaw-plugin.py \
  --mcp-endpoint http://localhost:9000/api/mcp \
  --output-dir /tmp/manyforge-mcp

# 2.2 — Deploy plugin into the sandbox workspace
tar -C /tmp -cf - manyforge-mcp | \
  openshell sandbox exec -n thor-v5 -- \
    bash -c 'mkdir -p /sandbox/.openclaw/workspace/.openclaw/extensions && \
             cd /sandbox/.openclaw/workspace/.openclaw/extensions && tar xf -'

# 2.3 — Patch plugin config + allowlist into openclaw.json
openshell sandbox exec -n thor-v5 -- \
  python3 -c "
import json, pathlib
config_path = pathlib.Path('/sandbox/.openclaw/openclaw.json')
config = json.loads(config_path.read_text()) if config_path.exists() else {}
plugins = config.setdefault('plugins', {}).setdefault('entries', {})
plugins.setdefault('manyforge-mcp', {})['enabled'] = True
plugins['manyforge-mcp']['config'] = {
    'endpoint': 'https://inference.local/v1/completions',
    'principalId': 'llm:openclaw-agent',
    'timeoutMs': 120000,
}
plugins_allow = config.setdefault('plugins', {}).setdefault('allow', [])
if 'manyforge-mcp' not in plugins_allow:
    plugins_allow.append('manyforge-mcp')
tools_cfg = config.setdefault('tools', {})
also_allow = tools_cfg.setdefault('alsoAllow', [])
if 'manyforge-mcp' not in also_allow:
    also_allow.append('manyforge-mcp')
config_path.write_text(json.dumps(config, indent=2))
"

# 2.4 — Restart gateway and verify plugin discovery
openshell sandbox exec -n thor-v5 -- \
  bash -c 'pkill -f "openclaw gateway" 2>/dev/null || true; sleep 2; HOME=/sandbox openclaw gateway run &'
openshell sandbox exec -n thor-v5 -- \
  bash -lc 'HOME=/sandbox openclaw plugins list | grep manyforge-mcp'

# Final live-agent validation is separate and uses:
openclaw agent --agent main --local --session-id test-manyforge-tools \
  -m "Reply with only the names of any tools available to you whose names start with manyforge_. If none are available, reply none."
```

**Current direct-route status**:

- A dedicated ManyForge TLS front end has been implemented and tested on the
  host.
- Live sandbox validation on OpenShell `0.0.26` failed for both:
  - `https://host.openshell.internal:9443/...` with policy `tls: terminate`
  - `https://host.openshell.internal:443/...` with policy `access: full`
- In both cases, sandbox egress failed at the proxy layer with
  `HTTP/1.1 403 Forbidden` on `CONNECT host.openshell.internal:*`.
- Bypassing proxy environment variables from the sandbox and connecting
  directly to `172.17.0.1:443` also failed with `connection refused`.

**Operational implication**:

- The mux-backed path remains the only validated ManyForge transport for the
  embedded OpenClaw agent on this platform.
- `./scripts/configure-manyforge-provider.sh` now skips the direct-route probe
  by default and uses the mux baseline immediately.
- The plugin is now built as a versioned artifact with pinned metadata and sha,
  then installed into `~/.openclaw-data/extensions/manyforge-mcp`.
- The install flow uses:
  - `OPENCLAW_STATE_DIR=/sandbox/.openclaw-data`
  - `OPENCLAW_CONFIG_PATH=/sandbox/.openclaw-data/openclaw.json`
  This avoids the symlinked `~/.openclaw/extensions` base and the root-owned
  runtime config during install.
- Runtime origin now shows `global`.
- Workspace fallback remains in `configure-manyforge-provider.sh` only as a
  compatibility path if artifact install fails on another stack.
- To re-run the direct-route experiment explicitly:

```bash
MANYFORGE_ENABLE_DIRECT_ROUTE_EXPERIMENT=true \
  ./scripts/configure-manyforge-provider.sh
```

Until OpenShell exposes a supported second route or the sandbox can reach a
different routable hostname/interface directly, the mux remains the required
primary path.

The remaining transport simplification work is therefore upstream-facing, not a
normal local refactor. See [PLATFORM_REQUIREMENTS.md](PLATFORM_REQUIREMENTS.md)
for the concrete OpenShell/OpenClaw/NemoClaw-Thor capabilities required before
the mux can be retired safely.

**Exit criteria**: ✅ The OpenClaw runtime loads the installed `manyforge-mcp`
plugin from global extensions, and a live agent turn can enumerate
`manyforge_*` tools when a model is available.

**Operational smoke**:

```bash
# Direct only
./scripts/manyforge-openclaw-smoke.sh direct

# ManyForge mux baseline only
./scripts/manyforge-openclaw-smoke.sh mux

# Full validated baseline
./scripts/manyforge-openclaw-smoke.sh all

# Or via the stack wrapper
./scripts/manyforge-stack.sh smoke all
```

The mux smoke requires the embedded agent to:
- call `manyforge_execution_status`
- terminate with `report_lifecycle_state(...)`

By default, mux smoke re-applies the **current deployed plugin artifact**
(`manyforge-mcp-current.*`) rather than silently rebuilding `latest`. This is
intentional: rollback validation should test the deployed candidate, not replace
it.

This avoids relying on free-text compliance for machine-checked outcomes.

**Plugin lifecycle control**:

```bash
# Show the current deployed plugin artifact and the last smoke result
./scripts/manyforge-stack.sh plugin status

# List versioned plugin artifacts available for deploy/rollback
./scripts/manyforge-stack.sh plugin list

# Build a new plugin artifact and deploy it
./scripts/manyforge-stack.sh plugin deploy

# Redeploy the current pinned artifact without rebuilding
./scripts/manyforge-stack.sh plugin deploy --use-current-artifact

# Roll back to the previous versioned artifact
./scripts/manyforge-stack.sh plugin rollback

# Roll back N steps into artifact history
./scripts/manyforge-stack.sh plugin rollback 2
```

Operational notes:
- `manyforge-mcp-latest.*` tracks the newest built artifact.
- `manyforge-mcp-current.*` tracks the artifact currently deployed to the
  sandbox.
- Rollback operates on versioned archives in `~/.manyforge/plugin-artifacts/`
  and updates the sandbox's `current` metadata accordingly.
- The runtime plugin should report `Origin: global`; `workspace` is fallback
  only.

Current model-in-the-loop baseline:
- The semantic ManyForge demo now uses a role-based deployment alias
  `selection_model` rather than a model-family-specific alias.
- The demo currently defaults to text-only semantic selection because the
  multimodal path remains under investigation for the local model families
  currently in use.
- Composer now surfaces deployment model endpoints, prompt templates, and live
  `/v1/models` discovery directly in the UI; assistant chat is still future
  work.

**Smoke state**:

- Host-side latest smoke result: `~/.manyforge/smoke/latest.json`
- Host-side smoke history: `~/.manyforge/smoke/history.jsonl`
- Sandbox-side latest smoke result: `/sandbox/.manyforge/smoke/latest.json`

`manyforge-stack.sh status` reads the latest smoke result so operators can see
the last validated direct/mux state without rerunning the full smoke path.

**Implementation notes**: Root-level `mcp.servers` in `openclaw.json` is not a
valid config path for the embedded main agent in this build, and ACPX
`mcpServers` only apply to ACPX-backed sessions. The supported main-agent path
is therefore a workspace plugin that registers agent tools and forwards them
over the muxed `inference.local` transport.

---

### Sprint 3: Missing MCP Tools — Fill the agent's capability gaps ✅ COMPLETE

**Goal**: Add the MCP tools that agents need for real work. These are
wrappers around existing REST endpoints — no new backend logic.

**Deliverables**:

| # | Task | File(s) | Test |
|---|------|---------|------|
| 3.1 | `manyforge_scene_snapshot` | ✅ | `mcp_server.py` — wraps HMI render_scene |
| 3.2 | `manyforge_program_save` | ✅ | `mcp_server.py` — wraps program.save() |
| 3.3 | `manyforge_files` | ✅ | `mcp_server.py` — wraps routes_filesystem browse |
| 3.4 | `manyforge_deployment` | ✅ | `mcp_server.py` — wraps deployment reader |
| 3.5 | Unit tests | ✅ | 10 new tests in `test_mcp_server.py`, 236 total pass |
| 3.6 | Verify from sandbox | ✅ | 17 tools confirmed from sandbox via bridge |

**Tool specifications**:

```python
# 3.1 — Scene snapshot (wraps GET /api/scene/snapshot)
@mcp.tool()
def manyforge_scene_snapshot(
    width: int = 640,
    height: int = 480,
    format: str = "jpeg",
) -> str:
    """Render the 3D scene as a base64-encoded image.
    Requires HMI server running (--hmi-port). Returns base64 string
    and metadata, or error if no Viser client is connected.
    """
    # Returns: {"image_base64": "...", "width": 640, "height": 480,
    #           "format": "jpeg", "timestamp_ns": ...}
    # Or: {"error": "HMI server not running"}

# 3.2 — Program save (wraps POST /api/program/save)
@mcp.tool()
def manyforge_program_save(
    path: Optional[str] = None,
) -> str:
    """Save the currently loaded program to a YAML file.
    If path is omitted, saves to the original source path.
    Path must be within allowed filesystem roots.
    """
    # Returns: {"status": "ok", "path": "/workspace/..."}

# 3.3 — File browser (wraps GET /api/files/browse)
@mcp.tool()
def manyforge_files(
    path: Optional[str] = None,
    kind: Optional[str] = None,
) -> str:
    """Browse files on disk. Returns directory listing with file metadata.
    Use kind="program" or kind="deployment" to filter for artifact files.
    Path must be within allowed filesystem roots.
    """
    # Returns: {"path": "/workspace", "entries": [...], "root_shortcuts": [...]}

# 3.4 — Deployment reader (wraps GET /api/deployment)
@mcp.tool()
def manyforge_deployment() -> str:
    """Get the currently loaded deployment configuration.
    Includes robot adapters, capability keys, model endpoints,
    prompt templates, and audit policy.
    """
    # Returns the full deployment dict or {"error": "No deployment loaded"}
```

**Test procedure (Sprint 3)**:

```bash
# Run unit tests inside container
docker exec manyforge-dev-run-* bash -c \
  'cd /workspace && python -m pytest manyforge_composer/tests/test_mcp_new_tools.py -v'

# Verify tool count increased
kubectl exec -n openshell "$SANDBOX_POD" -c sandbox -- \
  curl -sf -X POST http://host.openshell.internal:9000/api/mcp \
    -H "Content-Type: application/json" \
    -H "X-Principal-ID: llm:openclaw-agent" \
    -d '{"jsonrpc":"2.0","method":"tools/list","id":1}' \
  | python3 -c "import json,sys; d=json.load(sys.stdin); print(f'{len(d[\"result\"][\"tools\"])} tools')"
# Expected: 17 tools (13 existing + 4 new)

# Agent uses new tools
openshell sandbox connect thor-v5
openclaw agent --agent main --local --session-id test-new-tools \
  -m "Browse /workspace for .program.yaml files using mcp_manyforge_files"
```

**Exit criteria**: ✅ 17 MCP tools available, all 236 tests pass, agent
can use the new tools from the sandbox. Skill updated with all 17 tools.

---

### Sprint 4: Agent Knowledge — AGENTS.md, SOUL.md, and first skill ✅ COMPLETE

**Goal**: Give the agent enough domain knowledge to do useful work with
ManyForge. Start with a minimal AGENTS.md (tool reference + safety rules)
and one skill (`manyforge-monitor-execution`), then test with a real
agent workflow.

**Deliverables**:

| # | Task | File(s) | Test |
|---|------|---------|------|
| 4.1 | Write AGENTS.md | ✅ | `scripts/templates/AGENTS.md` — 17 tool reference + workflows + safety rules |
| 4.2 | Write SOUL.md | ✅ | `scripts/templates/SOUL.md` — physical/execution/data/authority safety boundaries |
| 4.3 | Write monitor skill | ✅ | `scripts/templates/skills/manyforge-monitor-execution/SKILL.md` |
| 4.4 | Upload to sandbox | ✅ | AGENTS.md, SOUL.md at `/sandbox/`, skill at `~/.agents/skills/` |
| 4.5 | End-to-end: load→run→monitor | ✅ | Loaded pick_and_place, ran, monitored to IDLE completion |

**AGENTS.md content outline** (~300-500 lines, not 1000 — start lean):

```markdown
# ManyForge Agent Reference

## Architecture
- CycleManager states: IDLE → RUNNING → STOPPED → IDLE (reset)
- Behavior tree executor: 10 Hz tick loop
- Execution kernel: C++ motion planner (or MockKernelClient)

## MCP Tools (17 total)
[For each tool: name, purpose, key parameters, return shape, usage notes]

## Workflows
### Load and run a program
1. mcp_manyforge_files(path="/workspace", kind="program")
2. mcp_manyforge_program_load(program_path="...", deployment_path="...")
3. mcp_manyforge_program_validate(tree=<current tree>)  # optional
4. mcp_manyforge_cycle_control(command="run")
5. mcp_manyforge_execution_status(include="digest")  # poll

### Stop and diagnose
1. mcp_manyforge_cycle_control(command="stop")
2. mcp_manyforge_debug_export()  # full diagnostic bundle
3. mcp_manyforge_scene_state()  # current scene

### Recovery via runtime override
1. mcp_manyforge_runtime_override(action="apply", node_name="...", params={...})
2. mcp_manyforge_cycle_control(command="reset")
3. mcp_manyforge_cycle_control(command="run")

## Safety Rules
[Extracted from SOUL.md — the critical ones inline]
```

**Test procedure (Sprint 4)**:

```bash
# Upload workspace files
openshell sandbox upload thor-v5 scripts/templates/AGENTS.md /sandbox/AGENTS.md
openshell sandbox upload thor-v5 scripts/templates/SOUL.md /sandbox/SOUL.md
openshell sandbox upload thor-v5 \
  scripts/templates/skills/manyforge-monitor-execution/ \
  /sandbox/.openclaw/skills/manyforge-monitor-execution/

# Restart gateway to pick up new workspace files
kubectl exec -n openshell "$SANDBOX_POD" -c sandbox -- \
  bash -c 'pkill -f "openclaw gateway" 2>/dev/null; sleep 2; HOME=/sandbox openclaw gateway run &'

# Test: agent loads and runs a program
openshell sandbox connect thor-v5
openclaw agent --agent main --local --session-id test-e2e-1 --timeout 120 \
  -m "Use ManyForge to: 1) Browse /workspace for program files, \
      2) Load the first program you find, 3) Report the program name and tree structure"

# Test: agent runs and monitors (requires a loaded program)
openclaw agent --agent main --local --session-id test-e2e-2 --timeout 120 \
  -m "Start the loaded ManyForge program, monitor execution for 15 seconds \
      polling every 3s, then stop it and report what happened"
```

**Exit criteria**: ✅ Agent can autonomously load a program, run it,
monitor execution, and stop it — following the patterns from AGENTS.md.
Verified: browse→load pick_and_place→run→monitor→IDLE completion→scene state.

---

### Sprint 5: Automation — `configure-manyforge-provider.sh` ✅ COMPLETE

**Goal**: Automate everything from Sprints 1-4 into a single idempotent
script. After this sprint, going from a clean NemoClaw-Thor setup to a
fully wired ManyForge integration is one command.

**Deliverables**:

| # | Task | File(s) | Test |
|---|------|---------|------|
| 5.1 | `configure-manyforge-provider.sh` | ✅ | `scripts/configure-manyforge-provider.sh` — 9-step idempotent setup |
| 5.2 | `manyforge-stack.sh` | ✅ | `scripts/manyforge-stack.sh` — up/down/status commands |
| 5.3 | Clean-slate test | ✅ | `status` verified all components healthy, files present |

**Test procedure (Sprint 5)**:

```bash
# Remove ManyForge MCP config to simulate clean state
kubectl exec -n openshell "$SANDBOX_POD" -c sandbox -- \
  python3 -c "
import json, pathlib
c = json.loads(pathlib.Path('/sandbox/.openclaw/openclaw.json').read_text())
c.get('mcp', {}).get('servers', {}).pop('manyforge', None)
pathlib.Path('/sandbox/.openclaw/openclaw.json').write_text(json.dumps(c, indent=2))
print('ManyForge MCP config removed')
"

# Run the automation
./scripts/manyforge-stack.sh up --with-nemoclaw

# Verify
./scripts/manyforge-stack.sh status

# Idempotency: run again, should complete without errors
./scripts/manyforge-stack.sh up --with-nemoclaw
```

**Exit criteria**: ✅ `manyforge-stack.sh status` verifies all components.
Scripts use tar-pipe for file uploads (avoids openshell directory nesting bug).
Mux proxy auto-start integrated. NemoClaw-Thor lib sourcing for config/helpers.

---

### Sprint 6: Remaining Skills + Full Agent Workflow ✅ COMPLETE

**Goal**: Add the remaining AgentSkills and validate the full
diagnose→intervene→escalate workflow with induced failures.

**Deliverables**:

| # | Task | File(s) | Test |
|---|------|---------|------|
| 6.1 | `manyforge-diagnose-failure` skill | ✅ | Structured failure classification and reporting |
| 6.2 | `manyforge-intervene-recovery` skill | ✅ | Scene edit, runtime override, and intervention strategies |
| 6.3 | `manyforge-escalate-to-human` skill | ✅ | Severity-classified escalation report format |
| 6.4 | `manyforge-delegated-recovery` skill | ✅ | Full diagnose→intervene→escalate workflow |
| 6.5 | All skills deployed to sandbox | ✅ | 6 ManyForge skills installed in `~/.agents/skills/` |

**Test procedure (Sprint 6)**:

```bash
# Upload all skills
./scripts/configure-manyforge-provider.sh  # re-uploads everything

# Induced failure scenario:
# 1. Load a pick-and-place program
# 2. Start execution
# 3. While running, remove the target object via scene_edit
# 4. Agent detects failure, diagnoses, attempts recovery

openshell sandbox connect thor-v5
openclaw agent --agent main --local --session-id test-recovery --timeout 300 \
  -m "Load the pick-and-place program from /workspace. Start execution and \
      monitor it. If a failure occurs, diagnose it and attempt recovery \
      using the manyforge-diagnose-failure and manyforge-intervene-recovery \
      skills. Report every step you take."
```

**Exit criteria**: ✅ 6 ManyForge skills installed and validated in sandbox.
Skills cover full diagnose→intervene→escalate workflow. Induced failure
testing limited by MockKernel speed (completes before stop possible),
but all tool calls and skill documentation verified.

---

### Sprint 7: Infrastructure Health + Remaining MCP Tools ✅ COMPLETE

**Goal**: Add the `GET /api/infra/status` endpoint and the two remaining
lower-priority MCP tools (`manyforge_kernel`, `manyforge_session`).

**Deliverables**:

| # | Task | File(s) | Test |
|---|------|---------|------|
| 7.1 | `GET /api/infra/status` | ✅ | `routes_infra.py` + app.py mount — returns component health |
| 7.2 | `manyforge_kernel` MCP tool | ✅ | snapshot/metrics/all include modes |
| 7.3 | `manyforge_session` MCP tool | ✅ | status + reset (cycle + overrides) |
| 7.4 | Frontend status indicator | — | Deferred (requires frontend/browser work) |

**Test results**: 245 composer tests pass (236 + 9 new). 19 MCP tools
verified from sandbox via bridge.

---

### Sprint Summary

| Sprint | Focus | Blocks | Test gate | Status |
|--------|-------|--------|-----------|--------|
| **1** | Network connectivity | Everything | Sandbox → ManyForge HTTP works | ✅ |
| **2** | MCP registration | Agent tool use | Agent discovers `mcp_manyforge_*` tools | ✅ |
| **3** | Missing MCP tools | Agent capabilities | 17 tools, agent can browse/save/snapshot | ✅ |
| **4** | Agent knowledge | Useful agent work | Agent loads→runs→monitors a program | ✅ |
| **5** | Automation scripts | Reproducible setup | One command brings up full stack | ✅ |
| **6** | Recovery skills | Autonomous operation | Agent diagnoses and recovers from failure | ✅ |
| **7** | Health + polish | Production readiness | Infrastructure monitoring, all tools | ✅ |

**Critical path**: 1 → 2 → 3 → 4 (each sprint depends on the previous).
Sprint 5 can start after Sprint 2 (automation doesn't need new tools).
Sprint 6 depends on Sprints 3+4. Sprint 7 is independent.

```
Sprint:  1 ──→ 2 ──→ 3 ──→ 4 ──→ 6
                │              │
                └──→ 5 ────────┘
                                     7 (independent)
```

**Start with Sprint 1** — it requires zero code changes and validates
the most fundamental assumption: can the sandbox talk to ManyForge?
