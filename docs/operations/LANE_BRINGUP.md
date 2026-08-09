# Lane bring-up — direct / openclaw / hermes

> **Cutover status:** retained as a restoration and behavior reference. N6-c5
> restores the bounded in-tree `direct` (`nemoclaw` alias) provider bridge and
> strict Composer client, but not this runbook's live model, Program-operation,
> launcher, OpenClaw, or Hermes workflows. The command recipes below are not
> qualified on the current `dev` branch and must not be used until N7 closes
> the product boundary and each selected external lane reaches its named gate.
> Present-tense descriptions below are historical unless a section explicitly
> identifies a current bounded component or retained offline procedure.

Authoritative historical reference for bringing up any of the three
**assistant lanes** and running the smoke corpus against it. The lane was selected by the
`ASSISTANT_PROVIDER` env var; everything else (vLLM, proxy, Composer) is
shared across lanes at the captured baseline.

> **Build-around invariant** — the host-side ManyForge repo
> (`src/manyforge`) and the editable sibling
> (`src/NemoClaw-Thor/manyforge`) are the only trees we modify. The
> pristine `~/NemoClaw` checkout is never touched. The Hermes lane and the
> OpenClaw bridge live under NemoClaw-Thor; the direct bridge lives in this
> repo.

> **Maintenance rule** — if you change `scripts/lib/assistant.sh`,
> `scripts/lib/compose.sh`, `scripts/setup-direct.sh`,
> `scripts/setup-hermes.sh`, or the bridge ports, update this file in the
> same commit.

______________________________________________________________________

## The three lanes at a glance

|                          | **direct** (`nemoclaw` alias)                                          | **openclaw**                                                                                                                                                                                                                                                 | **hermes**                                                                                                                  |
| ------------------------ | ---------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------- |
| `ASSISTANT_PROVIDER`     | `direct`                                                               | `openclaw`                                                                                                                                                                                                                                                   | `hermes`                                                                                                                    |
| Bridge port              | `:8100`                                                                | `:8200`                                                                                                                                                                                                                                                      | `:8300`                                                                                                                     |
| Bridge process           | in-tree `manyforge_assistant_bridge.bridge` (host venv)                | NemoClaw-Thor `start-openclaw-assistant-bridge.sh` (CLI shell-out)                                                                                                                                                                                           | NemoClaw-Thor `lanes/hermes/service.py` (uvicorn)                                                                           |
| Tool surface             | inline `tools[]` catalog in the bridge prompt                          | gateway discovery (`tool_search`/`tool_describe`)                                                                                                                                                                                                            | native MCP tools (`mcp_manyforge_*`)                                                                                        |
| Sandbox                  | none — talks straight to vLLM                                          | OpenClaw gateway in `openshell-my-assistant-*` (`:18789` in-sandbox)                                                                                                                                                                                         | Hermes `/v1/runs` gateway in `openshell-hermes-*` (forwarded to host `:8642`)                                               |
| Smoke `--self-heal-lane` | `state-only`                                                           | `openclaw`                                                                                                                                                                                                                                                   | `hermes`                                                                                                                    |
| Enable gate              | none                                                                   | none                                                                                                                                                                                                                                                         | `HERMES_LANE_PHASE4_ENABLED=true`                                                                                           |
| First-time provisioning  | `scripts/setup-direct.sh` (host venv only — no sandbox, no onboarding) | one-time `nemoclaw onboard` creates the `my-assistant` sandbox (see `NemoClaw-Thor/USER_QUICKSTART_MANUAL.md` — some onboard defaults need the local-provider fixups), then `NemoClaw-Thor/manyforge/setup-manyforge-assistant.sh` applies the merged policy | `scripts/setup-hermes.sh <sandbox>` provisions its own born-with-MCP sandbox — `nemoclaw onboard` is NOT used for this lane |

> **Lane dev/analysis** (cross-lane numbers, speed, scorer-strictness) lives in
> the sibling `NemoClaw-Thor` repo: `manyforge/docs/LANE-COMPARISON.md`; the full
> scorer note is archived alongside it at
> `manyforge/docs/archive/THREE-LANE-SCORER-NOTE.md`.

Set checkout paths once in each operator shell; override either variable when
your layout differs:

```bash
export MANYFORGE_ROOT="${MANYFORGE_ROOT:-${HOME}/workspaces/dev_ws/src/manyforge}"
export NEMOCLAW_THOR="${NEMOCLAW_THOR:-${HOME}/workspaces/dev_ws/src/NemoClaw-Thor}"
```

**Shared foundation (all lanes):**

| Component                           | Port    | Notes                                                                                            |
| ----------------------------------- | ------- | ------------------------------------------------------------------------------------------------ |
| vLLM (`manyforge-e2e-vllm`)         | `:8050` | `MODEL_PROFILE`, e.g. `gemma4-12b-it-gguf`. Cold load can take 60+ min.                          |
| vllm-proxy (`vllm-proxy.py`)        | `:8000` | Token/trim mutation; upstream for every bridge.                                                  |
| Composer (`manyforge-e2e-composer`) | `:9000` | The smoke runner's target. Dispatches each assistant turn to `MANYFORGE_ASSISTANT_ENDPOINT_URL`. |

The Composer reads two env vars **at process start** to decide which bridge
it talks to:

```
MANYFORGE_ASSISTANT_PROVIDER=<direct|openclaw|hermes>
MANYFORGE_ASSISTANT_ENDPOINT_URL=http://127.0.0.1:<8100|8200|8300>/v1/manyforge/assistant
```

Switching lanes therefore means **re-pointing the Composer** (a container
restart) and **starting the target lane's bridge**.

______________________________________________________________________

## ⚠️ vLLM reload trap — read before switching lanes

`scripts/launch.sh stop` and `restart` both run `docker rm -f manyforge-e2e-vllm` (`scripts/lib/assistant.sh:902`). On the next `start`,
`start_model` cold-loads the model again — **60+ minutes** for a fresh
gemma4-12b QAT load. `start_model` *was* idempotent when the container was
already up serving the right `MODEL_PROFILE`
(`scripts/lib/assistant.sh:267`), but `launch.sh start` refuses to run while
launcher-managed components are up (captured `scripts/lib/preflight.sh`, blob
`ffdaace61ab3c29a8ebe50dc4c17d99632146eea`, now deleted), so the
only launcher path to a lane switch is `restart` → which kills vLLM.

**Two supported paths:**

1. **Cold boot (first bring-up of the day):** use `launch.sh` — it brings up
   everything including vLLM. See [Canonical launch.sh path](#canonical-launchsh-path-cold-boot).
1. **Lane switch with vLLM already loaded (the common case):** use the
   **surgical switch** below — it leaves vLLM + proxy untouched and only
   re-points the Composer + swaps the bridge. This is what the smoke matrix
   uses to compare lanes back-to-back on the same loaded model.

______________________________________________________________________

## Surgical lane switch (preserves the loaded vLLM)

Run from the manyforge repo root: `cd "${MANYFORGE_ROOT}"`.

### Step 0 — sanity: shared foundation is up

```bash
# vLLM serving the expected model, through the proxy:
curl -sf http://127.0.0.1:8000/v1/models | python3 -c 'import sys,json;print([m["id"] for m in json.load(sys.stdin)["data"]])'
# Composer healthy:
curl -sf http://127.0.0.1:9000/api/health
```

If either is down, you need the [cold boot](#canonical-launchsh-path-cold-boot) path first.

### Step 1 — stop whatever bridge is currently bound

Only one lane's bridge should be running at a time (they bind distinct
ports, but keeping one live avoids confusion). Stop by PID:

```bash
for p in 8100 8200 8300; do
  pid=$(ss -ltnp 2>/dev/null | grep ":$p " | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
  [ -n "$pid" ] && { echo "stopping bridge :$p (pid $pid)"; kill "$pid"; }
done
# If the hermes lane was up, also stop its host→sandbox forward:
#   pkill -f 'openshell forward .*--local 8650'  (or kill the openshell PID on :8650)
```

### Step 2 — start the target lane's bridge

Pick exactly one:

<details><summary><b>direct</b> — bridge on :8100 (host venv, no sandbox)</summary>

```bash
cd "${MANYFORGE_ROOT}"
mkdir -p /tmp/manyforge-assistant-e2e
env \
  BRIDGE_HOST=127.0.0.1 BRIDGE_PORT=8100 \
  DIRECT_LANE_HISTORY_CHAR_BUDGET=393216 \
  BRIDGE_UPSTREAM_BASE_URL=http://127.0.0.1:8000/v1 \
  BRIDGE_MANYFORGE_BASE_URL=http://127.0.0.1:9000 \
  BRIDGE_MODEL=gemma4-12b-it-gguf \
  BRIDGE_REQUEST_TIMEOUT_S=300 BRIDGE_MAX_WALL_TIME_S=300 BRIDGE_MAX_TURNS=16 \
  BRIDGE_AUDIT_LOG=/tmp/manyforge-assistant-e2e/direct-bridge-audit.jsonl \
  MANYFORGE_THOR_ROOT="${NEMOCLAW_THOR}/manyforge" \
  MANYFORGE_STATE_CONTEXT=first_then_every_n MANYFORGE_STATE_CONTEXT_EVERY_N=3 \
  BRIDGE_STATE_CONTEXT=first_then_every_n BRIDGE_STATE_CONTEXT_EVERY_N=3 \
  setsid manyforge_assistant_bridge/.venv/bin/python -m manyforge_assistant_bridge.bridge \
  </dev/null >/tmp/manyforge-assistant-e2e/direct-bridge.log 2>&1 &
```

- `DIRECT_LANE_HISTORY_CHAR_BUDGET=393216` = 75% of the 131072-token context
  (`ctx * 3`), matching what the launcher auto-sizes (`assistant.sh:573`).
- `MANYFORGE_THOR_ROOT` makes the direct lane use the **shared projection**
  core (`common/projection`), so program/scene snapshots match the other
  lanes. Verify: `_USE_SHARED_PROJECTION` must be `True`.
- Health: `curl -sf http://127.0.0.1:8100/healthz` → `"status":"ok"`.

If the venv is missing, run `scripts/setup-direct.sh` (it provisions
`manyforge_assistant_bridge/.venv` and verifies the lane end-to-end).

</details>

<details><summary><b>openclaw</b> — bridge on :8200 (revives the gateway in the my-assistant sandbox)</summary>

The OpenClaw bridge shell-outs to `openclaw agent`, which needs the gateway
listening on `:18789` inside the `openshell-my-assistant-*` sandbox. The
launcher's `start_bridge_openclaw` starts that gateway lazily; to reproduce
it surgically:

```bash
cd "${NEMOCLAW_THOR}"

# 1. ensure the gateway is up in the sandbox
SB=$(docker ps --format '{{.Names}}' | grep -E '^openshell-my-assistant-' | head -1)
docker exec "$SB" sh -c "ss -tlnp 2>/dev/null | grep -q ':18789'" \
  || docker exec -d "$SB" bash -c 'cd /sandbox; exec openclaw gateway --allow-unconfigured --bind loopback --auth token >/sandbox/openclaw-gateway.log 2>&1'

# 2. start the bridge
env \
  OPENCLAW_ASSISTANT_USE_GATEWAY=false \
  OPENCLAW_ASSISTANT_AGENT=manyforge-composer OPENCLAW_ASSISTANT_LOCAL=false \
  OPENCLAW_ASSISTANT_TIMEOUT_S=300 \
  OPENCLAW_ASSISTANT_BRIDGE_HOST=127.0.0.1 OPENCLAW_ASSISTANT_BRIDGE_PORT=8200 \
  OPENCLAW_ASSISTANT_COMPACT_EVERY_N=2 OPENCLAW_ASSISTANT_COMPACT_TIMEOUT_S=120 \
  OPENCLAW_ASSISTANT_METRICS_ENABLED=false \
  OPENCLAW_ASSISTANT_LOOP_TOOL_THRESHOLD=5 OPENCLAW_ASSISTANT_LOOP_ARGS_THRESHOLD=2 \
  OPENCLAW_ASSISTANT_TOOL_SURFACE=tools \
  MANYFORGE_STATE_CONTEXT=first_then_every_n MANYFORGE_STATE_CONTEXT_EVERY_N=3 \
  setsid ./manyforge/start-openclaw-assistant-bridge.sh \
  </dev/null >/tmp/manyforge-assistant-e2e/openclaw-bridge.log 2>&1 &
```

- `OPENCLAW_ASSISTANT_USE_GATEWAY=false` is required: this OpenClaw build does
  not expose `/v1/chat/completions` on the gateway HTTP server, so the bridge
  uses CLI shell-out (`assistant.sh:766`).
- `OPENCLAW_ASSISTANT_TOOL_SURFACE=tools` is the production surface (the
  `code` surface is for the multi-model bake-off only).
- Health: `curl -sf http://127.0.0.1:8200/healthz` → `"provider":"openclaw"`.
  Becomes 200 once the gateway is responsive.

Sandbox provisioning — `NEMOCLAW_THOR/manyforge/setup-manyforge-assistant.sh my-assistant` installs the composer skill, egress preset, `manyforge` MCP
server, and the `manyforge-composer` agent profile. It is **idempotent** —
re-run it to repair a degraded sandbox (gateway died, skill/agent lost). Two
gotchas:

- **A program must be loaded first** (Step 3 above) — the provisioner's
  compatibility check hits `/api/assistant/modes/composer-assistant`, which
  404s with no program. Load the scenario program before running it.
- The provisioner does **not** leave the gateway running; the bridge-start
  block above is what brings the gateway up on `:18789`.

```bash
MANYFORGE_ROOT="${MANYFORGE_ROOT}" \
NEMOCLAW_THOR="${NEMOCLAW_THOR}" \
  "${NEMOCLAW_THOR}/manyforge/setup-manyforge-assistant.sh" my-assistant
```

</details>

<details><summary><b>hermes</b> — bridge on :8300 (forwards to the Hermes gateway sandbox)</summary>

Requires a born-with-MCP Hermes sandbox (created once by `setup-hermes.sh`)
and a host→sandbox forward so the bridge can reach the gateway at
`HERMES_BASE_URL`.

```bash
cd "${NEMOCLAW_THOR}/manyforge"

# 1. forward the sandbox gateway (internal :18642) to host :8650 if not already up
#    SB=$(docker ps --format '{{.Names}}' | grep -E '^openshell-hermes-' | head -1)
#    openshell forward service --target-port 18642 --local 8650 <hermes-sandbox-name> &

# 2. start the bridge
env \
  HERMES_BASE_URL=http://127.0.0.1:8650 \
  MANYFORGE_MODEL=gemma4-12b-it-gguf \
  MANYFORGE_PRINCIPAL=hermes-smoke \
  MANYFORGE_HERMES_RUN_TIMEOUT_S=300 \
  HERMES_STREAM_STALE_TIMEOUT=360 HERMES_STREAM_READ_TIMEOUT=360 HERMES_API_TIMEOUT=360 \
  MANYFORGE_HERMES_AUDIT_PATH=/tmp/manyforge-assistant-e2e/hermes-bridge-audit.jsonl \
  setsid lanes/hermes/.venv/bin/python -m uvicorn lanes.hermes.service:app \
  --host 127.0.0.1 --port 8300 \
  </dev/null >/tmp/manyforge-assistant-e2e/hermes-bridge.log 2>&1 &
```

- The stream timeouts are pinned **above** the 300 s case budget (= budget +
  60\) so the proxy/case-budget fire first and Hermes's stale-detector does not
  trigger a retry storm (`scripts/setup-hermes.sh`).
- Health: `curl -sf http://127.0.0.1:8300/healthz` → `"provider":"hermes"`.

Create the sandbox once (born-with-MCP):

```bash
HERMES_LANE_PHASE4_ENABLED=true \
  NEMOCLAW_VLLM_ENDPOINT=http://host.openshell.internal:8000/v1 \
  NEMOCLAW_VLLM_MODEL=gemma4-12b-it-gguf \
  "${MANYFORGE_ROOT}/scripts/setup-hermes.sh" hermes-smoke
```

</details>

### Step 3 — re-point the Composer at the new bridge

The Composer container bakes its provider env at `docker run`. Re-point by
removing it and recreating it via the tested lib function (an incremental,
build-cached rebuild — typically 1–3 min, **vLLM is not touched**):

```bash
cd "${MANYFORGE_ROOT}"
docker rm -f manyforge-e2e-composer
export MANYFORGE_ROOT=$PWD
export MANYFORGE_ASSISTANT_PROVIDER=<direct|openclaw|hermes>
export MANYFORGE_ASSISTANT_ENDPOINT_URL=http://127.0.0.1:<8100|8200|8300>/v1/manyforge/assistant
export MANYFORGE_ASSISTANT_TIMEOUT_S=300
source scripts/lib/compose.sh
start_composer_dev_container

# wait for health:
until curl -sf http://127.0.0.1:9000/api/health | grep -q '"status":"ok"'; do sleep 4; done
echo "composer ready"
```

**Load the scenario program** (a fresh composer starts with `program: none`). This
is REQUIRED for the openclaw provisioner and the `/api/assistant/modes/composer-assistant`
manifest endpoint — both 404 without a loaded program. (The smoke runner loads a
program per case, so the direct/hermes *smoke* works without this, but load it anyway
for a sane initial state matching `launch.sh --scenario`.)

```bash
curl -sf -X POST http://127.0.0.1:9000/api/program/load \
  -H 'Content-Type: application/json' \
  --data-binary '{"path":"/workspace/examples/pick_and_place_ur10e_robotiq.program.yaml","deploymentPath":"/workspace/examples/assistant_modes_scene_authoring.deployment.yaml","forceDiscardOverrides":true}'
# verify the manifest endpoint now answers:
curl -sf http://127.0.0.1:9000/api/assistant/modes/composer-assistant | python3 -c 'import sys,json;print("catalogHash",json.load(sys.stdin)["catalogHash"][:16])'
```

Confirm the wiring took:

```bash
cpid=$(ss -ltnp | grep ':9000 ' | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
tr '\0' '\n' < /proc/$cpid/environ | grep MANYFORGE_ASSISTANT_
```

### Step 4 — run the smoke corpus

```bash
cd "${NEMOCLAW_THOR}/manyforge/scripts/debug"
python3 -u smoke_corpus_runner.py \
  --composer http://127.0.0.1:9000 \
  --self-heal --self-heal-lane <state-only|openclaw|hermes> \
  --report /tmp/<lane>-baseline.json
```

- **Heal is always on.** It restores canonical state after a chained-step
  failure so cascades don't produce unrealistic downstream scores. `openclaw`
  also splices a golden turn into the JSONL transcript; `hermes`/`state-only`
  replay state only.
- Corpus = 75 cases (future-tier cases are auto-skipped → 66 scored).
- Per-case budget = 295 s (`MANYFORGE_SMOKE_DEFAULT_TIMEOUT_S`), under the
  Composer's 300 s wall.
- Scoring is by **Composer bridge-tool dispatches**
  (`/api/assistant/bridge/tools/{id}`). Gateway-internal discovery calls
  (OpenClaw `tool_search`/`tool_describe`, Hermes `catalog_read` served
  locally by the MCP bridge) are **not** counted — this keeps the lanes
  comparable.

______________________________________________________________________

## Canonical launch.sh path (cold boot)

From a clean machine (nothing running), the launcher brings up vLLM + proxy +
Composer + the selected bridge in one shot:

```bash
cd "${MANYFORGE_ROOT}"

# direct
ASSISTANT_PROVIDER=direct MODEL_PROFILE=gemma4-12b-it-gguf \
  ./scripts/launch.sh start --lane manyforge-only --assistant on \
  --scenario ur10e-scene-authoring --non-interactive --yes

# openclaw (default provider)
ASSISTANT_PROVIDER=openclaw MODEL_PROFILE=gemma4-12b-it-gguf \
  ./scripts/launch.sh start --lane manyforge-only --assistant on \
  --scenario ur10e-scene-authoring --non-interactive --yes

# hermes (opt-in gate)
HERMES_LANE_PHASE4_ENABLED=true ASSISTANT_PROVIDER=hermes \
  HERMES_BASE_URL=http://127.0.0.1:8642 OPENCLAW_ASSISTANT_SANDBOX=hermes-smoke \
  MODEL_PROFILE=gemma4-12b-it-gguf \
  ./scripts/launch.sh start --lane manyforge-only --assistant on \
  --scenario ur10e-scene-authoring --non-interactive --yes
```

- `--lane` here is the **compose lane** (`manyforge-only` vs `isaac-sim`), not
  the assistant lane. The assistant lane is `ASSISTANT_PROVIDER`.
- `start_model` reuses an already-loaded vLLM if it serves `MODEL_PROFILE`; a
  cold load is 60+ min.
- **Do not** use `launch.sh restart`/`stop` to switch lanes if you want to keep
  a loaded model — see the [reload trap](#%EF%B8%8F-vllm-reload-trap--read-before-switching-lanes).
  Use the surgical switch instead.

`scripts/setup-direct.sh` is an idempotent **direct-lane** health/repair helper
(verifies vLLM/proxy/Composer/venv/shared-core and tells you how to start the
bridge). `scripts/setup-hermes.sh` provisions the **hermes** sandbox.

______________________________________________________________________

## Quick status / teardown

```bash
# what's bound
ss -ltn | grep -E ':(9000|8000|8050|8100|8200|8300|8650)'
# bridge identity
for p in 8100 8200 8300; do curl -sf http://127.0.0.1:$p/healthz 2>/dev/null | head -c 120; echo; done
# sandboxes
docker ps --format '{{.Names}}\t{{.Status}}' | grep -E 'vllm|composer|openshell'

# teardown of one bridge (leaves vLLM/proxy/composer up):
kill $(ss -ltnp | grep ':8100 ' | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
# full teardown (KILLS vLLM — next start is a cold load):
./scripts/launch.sh stop
```
