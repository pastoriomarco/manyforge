# Assistant E2E Cookbook

This cookbook is the live operator recipe set for ManyForge assistant
workflows. It covers the current v0.1 path:

```text
Composer -> manyforge_assistant_bridge -> local vLLM
         <- Composer bridge callbacks <-
```

Use it for practical end-to-end assistant checks, model/profile bake-offs, and
new assistant workflow validation as ManyForge grows. It is not a replacement
for CI: when a workflow graduates from manual E2E check to regression coverage,
keep the recipe here and add the corresponding automated test or verification
matrix.

NemoClaw-Thor owns model serving. ManyForge owns the assistant provider
contract, mode catalog, bridge service, Composer callbacks, and this cookbook.

## How Recipes Evolve

When adding an assistant capability, add or update a recipe here with:

- the user-facing workflow being checked,
- required deployment/program/example files,
- exact boot and cleanup assumptions,
- the command or manual action,
- the visible pass/fail signal,
- where automated coverage lives once it exists.

Keep recipes practical. Prefer one focused observable outcome per recipe:
for example, "assistant adds a draft scene object and the operator can discard
it" is better than a large demo that hides which part failed.

## Prerequisites

- ManyForge repo: `${HOME}/workspaces/dev_ws/src/manyforge`
- NemoClaw-Thor repo:
  `${HOME}/workspaces/nemoclaw/src/NemoClaw-Thor`
- Docker image for the ManyForge dev lane is built: `manyforge-dev:latest`
- NemoClaw-Thor vLLM image/profile support is already set up.
- Thor memory protocol is followed before each model switch.

Current primary candidate profiles:

```text
cosmos-reason2-8b                  # current known-good single-model demo default
qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge
nemotron3-nano-omni-30b-a3b-nvfp4
```

## Base Recipe: Cold Boot

### 1. Free Memory Before Starting vLLM

Only run one vLLM model container at a time on Thor. Before starting or
switching profiles, stop the current vLLM container and run the NemoClaw-Thor
memory protocol:

```bash
docker rm -f manyforge-e2e-vllm 2>/dev/null || true
sync && sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
free -g
nvidia-smi
```

`free -g` should show roughly 110+ GB available on Thor before a large-profile
launch. The cache drop is required because vLLM can leave unified-memory pages
pinned after stop, crash, or a large model swap. Skipping this step can produce
misleading startup failures such as vLLM memory-profiling assertions.

### 2. Start The Local Model Server

```bash
cd ${HOME}/workspaces/nemoclaw/src/NemoClaw-Thor

THOR_DETACH=1 \
THOR_CONTAINER_NAME=manyforge-e2e-vllm \
./serving/start-model.sh cosmos-reason2-8b
```

For Qwen3.6 or Nemotron Omni comparison runs, replace the profile with:

```text
qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge
nemotron3-nano-omni-30b-a3b-nvfp4
```

Verify:

```bash
curl -s http://127.0.0.1:8000/v1/models | python3 -m json.tool
```

### 3. Configure NemoClaw/OpenShell

This keeps the NemoClaw/OpenShell environment aligned with the served model.
The ManyForge bridge talks directly to `127.0.0.1:8000/v1`, but the provider
route, sandbox runtime config, and warmup should still be updated.

```bash
cd ${HOME}/workspaces/nemoclaw/src/NemoClaw-Thor

./setup/configure-local-provider.sh cosmos-reason2-8b
./setup/status.sh
```

### 4. Start Composer With The Live Assistant Provider And Native Viewer

Use host networking so Composer can reach the host bridge on `127.0.0.1:8100`.
The Composer UI uses the native viewer by default. Add `--hmi-port 8081` only
when deliberately testing the legacy Viser fallback.

```bash
cd ${HOME}/workspaces/dev_ws/src/manyforge

docker rm -f manyforge-e2e-composer 2>/dev/null || true

docker run -d --rm \
  --name manyforge-e2e-composer \
  --network host \
  -v ${HOME}/workspaces/dev_ws/src/manyforge:/workspace \
  -v manyforge_build-cache:/tmp/manyforge-build \
  -w /workspace \
  -e MANYFORGE_ASSISTANT_PROVIDER=nemoclaw \
  -e MANYFORGE_ASSISTANT_ENDPOINT_URL=http://127.0.0.1:8100/v1/manyforge/assistant \
  -e MANYFORGE_ASSISTANT_TIMEOUT_S=300 \
  manyforge-dev:latest \
  bash -lc 'python -m manyforge_composer \
    --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
    --host 127.0.0.1 \
    --port 9000 \
    --assistant-provider nemoclaw \
    --assistant-endpoint http://127.0.0.1:8100/v1/manyforge/assistant \
    --assistant-timeout-s 300'
```

Verify:

```bash
curl -s http://127.0.0.1:9000/api/infra/status | python3 -m json.tool
```

Expected status after loading a program in the UI or through the smoke runner:

```text
composer.status = healthy
composer.cycle_state = idle
hmi.status = disabled
deployment.status = loaded
```

### 5. Install Bridge Dependencies

```bash
cd ${HOME}/workspaces/dev_ws/src/manyforge

python3 -m venv manyforge_assistant_bridge/.venv
manyforge_assistant_bridge/.venv/bin/pip install -r manyforge_assistant_bridge/requirements.txt
```

## Recipe: Manual Bridge Start

Use this when you want to interact with Composer manually while keeping the
bridge process visible in a terminal.

```bash
cd ${HOME}/workspaces/dev_ws/src/manyforge

BRIDGE_HOST=127.0.0.1 \
BRIDGE_PORT=8100 \
BRIDGE_UPSTREAM_BASE_URL=http://127.0.0.1:8000/v1 \
BRIDGE_MANYFORGE_BASE_URL=http://127.0.0.1:9000 \
BRIDGE_MODEL=cosmos-reason2-8b \
BRIDGE_REQUEST_TIMEOUT_S=180 \
BRIDGE_MAX_WALL_TIME_S=180 \
BRIDGE_MAX_TURNS=16 \
BRIDGE_UPSTREAM_MAX_TOKENS=32768 \
BRIDGE_UPSTREAM_ENABLE_THINKING=false \
BRIDGE_UPSTREAM_TEMPERATURE=0.2 \
BRIDGE_UPSTREAM_TOP_K=1 \
BRIDGE_AUDIT_LOG=/tmp/manyforge-assistant-e2e/manual-bridge-audit.jsonl \
manyforge_assistant_bridge/.venv/bin/python -m manyforge_assistant_bridge.bridge
```

Verify:

```bash
curl -s http://127.0.0.1:8100/healthz | python3 -m json.tool
```

## Recipe: Known-Good Assistant Demo Launcher

Use this when you want the current best single-model assistant path without
manually stitching together the model, Composer, HMI, and bridge. The launcher
defaults to `cosmos-reason2-8b`, the current preferred lightweight
single-model candidate for the known-good assistant lane.

The launcher has two assistant-provider lanes; **OpenClaw (`openclaw`) is the
default** for the known-good assistant workflow:

| Lane | Default? | Bridge | Latency (median/test, cosmos-reason2-8b) | Notes |
|---|---|---|---|---|
| `openclaw` | ✅ default | NemoClaw-Thor `openclaw_assistant_bridge` on `:8200` | ~25 s/test (P1 ~22 s, P2 ~25 s, P3 ~31 s; 2026-05-08) | Persistent gateway in NemoClaw `my-assistant` sandbox. Adds bounded-autonomy enforcement (per-tool audit with assistantMode + catalogHash + principal). Use for the known-good assistant lane and shared/multi-tenant deployments. Provisions the OpenClaw sandbox automatically (egress preset, skill, MCP server). |
| `nemoclaw` | backup | in-tree `manyforge_assistant_bridge` on `:8100` | ~17 s/test (P1 ~11 s, P2 ~17 s, P3 ~19 s; 2026-05-08) | Direct vLLM. Bridge runs the tool loop in-process; for compound prompts the bridge inlines a structured `programSnapshot` / `sceneSnapshot` extract into the user message before pinning the action tool. Useful for fast local iteration when sandbox isolation is not needed. |

Both lanes preserve the same bounded-autonomy contract on the ManyForge side
(mode-scoped MCP wrapper + `/api/assistant/bridge/tools/{toolId}` + audit envelope).
The direct lane is faster, but the known-good assistant demo defaults to
OpenClaw because it exercises the sandbox, skill, MCP, and bounded-autonomy
envelope used by the target runtime workflow.

```bash
cd ${HOME}/workspaces/dev_ws/src/manyforge

# default: OpenClaw sandboxed lane
./scripts/demo-assistant-known-good.sh preflight
./scripts/demo-assistant-known-good.sh start

# direct vLLM backup lane:
ASSISTANT_PROVIDER=nemoclaw ./scripts/demo-assistant-known-good.sh start
```

The known-good launcher defaults to `ASSISTANT_MAX_TURNS=16`. This is a bridge
agent-loop budget, not a strict tool-call count: each turn can include one or
more callback attempts. Keep an upper bound so malformed tool schemas or model
looping cannot run forever, but use a high enough value for the model to repair
invalid callback arguments. Override it when testing:

```bash
ASSISTANT_MAX_TURNS=8 ./scripts/demo-assistant-known-good.sh start
```

What it does (default — `openclaw` sandboxed lane):

1. stops stale `manyforge-e2e-vllm` state if needed and runs the Thor memory
   protocol before model launch,
2. starts `cosmos-reason2-8b` through NemoClaw-Thor
   `serving/start-model.sh`,
3. runs `setup/configure-local-provider.sh` for the same profile (which also
   spawns the OpenClaw gateway in its SSH-session network namespace),
4. **provisions the OpenClaw sandbox** by running
   `manyforge/setup-manyforge-assistant.sh` — applies the
   `manyforge-composer` egress preset (with `allowed_ips` covering the
   Docker-bridge subnet so the SSRF guard accepts vLLM resolution),
   removes the built-in `local-inference` preset (superseded by ours),
   stages the `manyforge-composer` skill, and registers the `manyforge`
   MCP server in the sandbox via `openclaw mcp set`. Skip with
   `PROVISION_OPENCLAW_SANDBOX=false` if the sandbox is already provisioned,
5. starts Composer on `http://127.0.0.1:9000` with the native Composer viewer,
   configured to use the `openclaw` provider at
   `http://127.0.0.1:8200/v1/manyforge/assistant`,
6. starts the OpenClaw assistant bridge (`openclaw_assistant_bridge`) on
   `http://127.0.0.1:8200`. The bridge translates Composer's
   `manyforge.assistant.provider_request.v0` envelope into
   `/v1/chat/completions` calls against the persistent gateway,
7. loads `pick_and_place_ur10e_robotiq.program.yaml` with
   `assistant_modes_scene_authoring.deployment.yaml`.

The direct backup lane (`ASSISTANT_PROVIDER=nemoclaw`) uses the same model
profile and Composer deployment but starts the in-tree
`manyforge_assistant_bridge` on `http://127.0.0.1:8100` and drives the tool
loop in-process against vLLM.

The preflight refuses to start if ports `8000`, `8080`, `8081`, `8100`,
`8200`, or `9000` are already owned by unmanaged processes. This prevents
accidental duplicate Composer, HMI, bridge (either lane), OpenShell, or vLLM
instances. **As of `c3c492c` the preflight is strict against
launcher-managed state too** — already-running launcher Composer / vLLM /
bridge / supervisor containers or PID files are reported as conflicts.
Use `scripts/launch.sh stop` or `scripts/launch.sh restart` to clear the
state before re-running `start`. See [`scripts/MIGRATION.md`](../../scripts/MIGRATION.md)
"Restart workflow" for the supported lifecycle.

### One-time prerequisites for the OpenClaw lane

You only need to do these once per machine (or after a NemoClaw upgrade):

1. **Onboard the `my-assistant` sandbox** via `nemoclaw onboard`. Pick
   "Other OpenAI-compatible endpoint" → `http://127.0.0.1:8000/v1` →
   served-model-name matching your profile slug; sandbox name
   `my-assistant`; policy preset `local-inference` (`setup-manyforge-assistant.sh`
   replaces it later with our combined `manyforge-composer` preset).
   Detailed walk-through:
   [`nemoclaw/src/NemoClaw-Thor/setup/NEMOCLAW-OPENCLAW-WORKFLOW.md`](../../../../../nemoclaw/src/NemoClaw-Thor/setup/NEMOCLAW-OPENCLAW-WORKFLOW.md).
2. **Verify `nemoclaw`, `openshell`, `docker`, `kubectl` are on PATH.**
   `manyforge/setup-manyforge-assistant.sh` checks `nemoclaw` and `docker`
   explicitly; `kubectl` is invoked through `docker exec` against the
   OpenShell cluster container.

After that, every demo cycle is a single
`./scripts/demo-assistant-known-good.sh start`. The integration design
and all decisions made in landing it live at
[`nemoclaw/src/NemoClaw-Thor/manyforge/docs/MANYFORGE-MCP-INTEGRATION.md`](../../../../../nemoclaw/src/NemoClaw-Thor/manyforge/docs/MANYFORGE-MCP-INTEGRATION.md).

During startup the launcher waits on real readiness endpoints without showing a
fake percentage or timeout-budget countdown. In an interactive terminal it
refreshes a small block in place with `Loading, please wait...` and the latest
three vLLM/Composer log lines. The final success signal is the
`Known-good assistant demo is ready` block with Composer, HMI, bridge, and model
details. If model startup takes a long time, inspect the full log in another
terminal with `docker logs -f manyforge-e2e-vllm`.

Then open Composer and use the manual UI recipe below. For automated
qualification of the same stack:

```bash
./scripts/demo-assistant-known-good.sh smoke
```

After Composer or bridge code changes, restart the stack and reload the
known-good program/deployment:

```bash
./scripts/demo-assistant-known-good.sh restart
```

Note: `restart-bridge` and `reload-composer` were partial-restart verbs in
the previous version of this script. They are now deprecated and map to a
full `restart` via `scripts/launch.sh`. **As of `c3c492c`, `start` is
strict against partial launcher-managed state** — the previously-supported
"`pkill` the bridge then re-`start` and let preflight reuse the still-
running vLLM / sandbox" pattern no longer works; preflight reports the
still-running components as conflicts and asks you to use `restart`
instead. See `scripts/MIGRATION.md` "Restart workflow" for the supported
lifecycle. The `REBUILD_FRONTEND_ON_COMPOSER_RELOAD` env var no longer
applies; the dev-container startup builds the frontend from source on
every launch (see `scripts/lib/compose.sh:start_composer_dev_container`).

Stop and free memory:

```bash
./scripts/demo-assistant-known-good.sh stop
```

The unified launcher's `stop` verb (which the shim now delegates to)
runs `sync` + `echo 3 > /proc/sys/vm/drop_caches` automatically after
the vLLM container is removed, so unified memory on Thor returns to
the OS without a separate manual step. This requires passwordless
sudo for the drop-caches kernel write; if sudo would prompt, the
launcher warns once and continues without freeing — operators on
Thor should add the following sudoers rule on first setup:

```
# /etc/sudoers.d/manyforge-drop-caches  (owner root, mode 0440)
%docker ALL=(root) NOPASSWD: /bin/sh -c echo 3 > /proc/sys/vm/drop_caches
```

Set `DROP_CACHES=false` to disable the auto-cleanup (useful for CI
hosts where the cache pressure isn't a concern).

`stop` preserves `openshell-cluster-nemoclaw` by default because that gateway
owns live OpenShell sandbox state, including a `my-assistant` sandbox created by
NemoClaw onboarding. To intentionally delete and recreate the gateway, use:

```bash
PRESERVE_OPENSHELL=false ./scripts/demo-assistant-known-good.sh stop
```

Comparison runs can override the served model while keeping the same demo
surface:

```bash
MODEL_PROFILE=qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge \
  ./scripts/demo-assistant-known-good.sh restart

MODEL_PROFILE=nemotron3-nano-omni-30b-a3b-nvfp4 \
  ./scripts/demo-assistant-known-good.sh restart
```

## Recipe: Manual Composer UI Demo

Use this after the base boot and manual bridge start when checking the actual
operator workflow.

1. Open `http://127.0.0.1:9000`.
2. Load `/workspace/examples/pick_and_place_ur10e_robotiq.program.yaml`.
3. Load `/workspace/examples/assistant_modes_scene_authoring.deployment.yaml`.
4. Confirm the native viewer in the Composer UI shows the robot/scene.
5. Open the lower-right assistant button.
6. Confirm the overlay shows:
   - configured provider `nemoclaw`,
   - deployment provider if different from the configured provider,
   - endpoint alias and model/profile from the deployment,
   - active mode and allowed-entry state,
   - request status and elapsed time while a request is running.
7. In `Query` mode, ask:

   ```text
   Is a program loaded? Read the current program and summarize it.
   ```

   Pass signal: the assistant returns a text answer, tool calls are listed in
   the side panel with expandable arguments/results, and Composer remains
   unmodified.

8. In `Composer Assistant` mode, ask for a small draft scene edit, for example:

   ```text
   Add a small blue box named ui_demo_box at x=0.42, y=-0.12, z=0.18.
   ```

   Pass signal: Composer's normal draft/apply state becomes active, the
   assistant transcript attributes the completed tool call, and the native
   viewer reflects the draft scene after Composer refreshes. There is no
   separate LLM approval layer; use the normal Composer apply/save/reload
   controls to keep or reject the change.

If you are validating the legacy Viser fallback and the Viser panel says HMI is
unavailable, Composer was not launched with `--hmi-port 8081` or no program has
been loaded yet. Restart Composer with the fallback flag, then reload the
program/deployment.

## Recipe: Automated Assistant E2E Checks

The reusable runner starts and stops the bridge for each sampling profile. It
loads the assistant-mode deployment and a baseline program, then exercises:

- direct vLLM tool-call probing,
- query-mode `program_read`,
- operator-facing Composer assistant flow with provider discovery, HMI status,
  draft mutation, request status, diagnostics, and discard,
- composer-assistant `scene_draft_add_object`,
- golden-scene `scene_draft_upsert_objects` with expected object IDs, shapes,
  and poses,
- composer-assistant full draft tool surface: discovery reads, scene remove,
  tree insert/update/delete/move/replace, parameter edits, and blackboard
  namespace/key edits,
- Composer request IDs and `/api/assistant/requests/{requestId}` status polling,
- Composer-to-bridge cancellation through `/api/assistant/requests/{requestId}/cancel`,
- draft discard by program reload,
- negative fail-closed and structured-error cases unless skipped.

Primary qualification:

```bash
cd ${HOME}/workspaces/dev_ws/src/manyforge

./scripts/smoke-assistant-e2e.py \
  --profile cosmos-bridge-default \
  --strict \
  --output /tmp/manyforge-assistant-e2e/assistant-primary.json
```

Use `--require-hmi` only for explicit legacy Viser fallback qualification.

Model/profile bake-off:

```bash
./scripts/smoke-assistant-e2e.py \
  --profile all \
  --output /tmp/manyforge-assistant-e2e/profile-bakeoff.json
```

Available runner profiles:

| Profile | Purpose |
|---|---|
| `vllm-generation-config` | Direct vLLM probe with no per-request generation overrides |
| `bridge-default` | Bridge defaults: `temperature=0.0`, `max_tokens=512`, thinking unset |
| `cosmos-bridge-default` | Current known-good Cosmos bridge profile: `temperature=0.0`, `max_tokens=8192` |
| `omni-vendor-tool` | NVIDIA tool-call sampling: `temperature=0.6`, `top_p=0.95`, `max_tokens=512`, thinking unset |
| `omni-vendor-tool-no-think` | Vendor sampling with `enable_thinking=false`, `max_tokens=512` |
| `omni-vendor-tool-no-think-32768` | Vendor sampling with `enable_thinking=false`, `max_tokens=32768` |
| `omni-instruct-no-think` | Instruct sampling: `temperature=0.2`, `top_k=1`, `max_tokens=512`, `enable_thinking=false` |
| `omni-instruct-no-think-128` | Output-budget sweep with `max_tokens=128` |
| `omni-instruct-no-think-256` | Output-budget sweep with `max_tokens=256` |
| `omni-instruct-no-think-1024` | Output-budget sweep with `max_tokens=1024` |
| `omni-instruct-no-think-32768` | Legacy Nemotron Omni realistic assistant profile: `max_tokens=32768`, `enable_thinking=false` |
| `omni-deterministic-no-think` | Deterministic non-thinking comparison |

## E2E Recipes

### Composer chat HTTP contract — required fields

> If you arrived here because chat returned a mock voice, see the
> short version in [`TROUBLESHOOTING.md` §"Assistant fell back to
> mock"](TROUBLESHOOTING.md#assistant-fell-back-to-mock); this section
> is the full contract reference.

Composer's chat endpoint (`POST /api/assistant/chat`) silently falls back
to **mock mode** if the request body omits the right fields. Operators
testing the live provider end-to-end must pass *both* of:

```json
{
  "message": "<your prompt>",
  "mode": "provider",
  "assistantMode": "composer-assistant"
}
```

Why each field matters:

- `mode` selects the dispatch lane. Valid values are `"mock"` and
  `"provider"`. Omitting it (or sending `"mock"`) hits the built-in
  scaffold provider that returns a canned warning and never invokes
  the configured external provider, regardless of
  `configuredProviderId`. The chat response carries
  `providerId: "mock"` in that case — a fast tell.
- `assistantMode` selects the deployment-defined bounded-autonomy mode
  (`composer-assistant`, `query`, etc.). Without it, the request is
  rejected at the contract layer because the deployment exposes
  multiple modes.

Quick check that a chat request actually reached the configured provider:

```bash
curl -sS -X POST http://127.0.0.1:9000/api/assistant/chat \
  -H 'Content-Type: application/json' \
  -d '{"message":"reply with just OK","mode":"provider","assistantMode":"composer-assistant"}'
```

Then poll status:

```bash
curl -sS http://127.0.0.1:9000/api/assistant/requests/<requestId> | jq .providerId
```

A correct setup returns the configured provider id (e.g.
`"openclaw"`). `"mock"` means the request was missing one of the two
fields above.

### Read-Only Query

Goal: the assistant reads current Composer state without mutating the draft.

Command:

```bash
./scripts/smoke-assistant-e2e.py \
  --profile cosmos-bridge-default \
  --skip-negative \
  --output /tmp/manyforge-assistant-e2e/query-and-draft.json
```

Pass signal: the `queryReadOnly` result contains a completed `program_read`
tool call, `requestStatus.status: completed`, and `draftMutated: false` in the
bridge audit.

### Draft Scene Edit

Goal: the assistant edits the in-memory Composer draft directly, while the
program remains a draft that can be discarded before run/adoption.

Pass signal: `composerDraftAdd.ok: true`, `draftMutated: true`, and the named
object appears in the draft before reload. The request status should be
`completed` with `draftMutated: true`.

Operator check: Composer's normal draft/apply state is the approval boundary.
The overlay should only attribute the assistant turn and tool calls that changed
the draft. The operator rejects the edit by discarding/reloading the draft, not
by accepting a separate LLM-only shadow draft.

### Operator Composer Flow

Goal: verify the same path an operator sees in the Composer UI: configured
provider discovery, active mode, HMI availability, assistant draft mutation,
request-status visibility, tool-call attribution, and normal discard.

Command:

```bash
./scripts/smoke-assistant-e2e.py \
  --profile cosmos-bridge-default \
  --require-hmi \
  --skip-negative \
  --output /tmp/manyforge-assistant-e2e/operator-flow.json
```

Pass signal: `operatorComposerFlow.ok: true`, `providerCatalog.configuredProviderId`
matches the configured provider, `hmi.status: healthy`, exactly one
`scene_draft_upsert_objects` call completes, no tool call is outside
`composer-assistant`, the draft program contains the expected object before
reload, HMI diagnostics match when available, request status is `completed`,
and reload/discard removes the assistant-authored object.

### Golden Scene Upsert

Goal: the assistant reproduces a small expected scene from text by calling
`scene_draft_upsert_objects` once. This verifies more than transport: it checks
that the resulting draft contains the expected object IDs, shape types,
dimensions/radius, frame, and positions.

Pass signal: `goldenSceneUpsert.ok: true`, exactly one completed
`scene_draft_upsert_objects` call, no `validationErrors`, both expected objects
appear in `matchedExpectedObjects`, `actualUpsertObjectIds` has no extras, and
reload/discard removes the assistant-authored draft objects. The validator checks
tool-call arguments and resulting draft state for object IDs, shape
type/dimensions/radius, frame, position, and orientation. It also checks the
live `/api/program` draft, request-status metadata, and that the assistant text
mentions the expected object IDs.

### Golden Tree Insert

Goal: the assistant inserts a precise behavior-tree node into the Composer draft
from text by calling `tree_draft_insert_node` once.

Pass signal: `goldenTreeInsert.ok: true`, exactly one completed
`tree_draft_insert_node` call, no `validationErrors`, and reload/discard removes
the assistant-authored node. The validator checks parent, insertion index, node
ID/name, node parameters, resulting draft-tree placement in the tool response
and live `/api/program`, request-status metadata, and assistant text mention of
the inserted node name.

### Golden Parameter Upsert

Goal: the assistant inserts a precise program parameter into the Composer draft
from text by calling `program_draft_upsert_parameters` once.

Pass signal: `goldenParameterUpsert.ok: true`, exactly one completed
`program_draft_upsert_parameters` call, no `validationErrors`, and
reload/discard removes the assistant-authored parameter. The validator checks
tool-call arguments, resulting draft parameter metadata in the tool response
and live `/api/program`, request-status metadata, and assistant text mention of
the parameter name.

### Golden Blackboard Upsert

Goal: the assistant inserts a precise blackboard namespace and key into the
Composer draft from text by calling `blackboard_draft_upsert_namespaces` and
`blackboard_draft_upsert_keys`.

Pass signal: `goldenBlackboardUpsert.ok: true`, exactly one completed call for
each blackboard tool, no `validationErrors`, and reload/discard removes the
assistant-authored namespace and key. The validator checks tool-call arguments
and resulting draft blackboard metadata in the tool response and live
`/api/program`, request-status metadata, and assistant text mention of the
namespace and key IDs.

### Draft Discard

Goal: assistant edits are not silently persisted.

Pass signal: `discardByReload.ok: true` and
`sceneContainsObjectAfterReload: false`.

### Fail-Closed Mode Enforcement

Goal: malformed or out-of-mode requests are rejected before the model can act.

Command:

```bash
./scripts/smoke-assistant-e2e.py \
  --profile cosmos-bridge-default \
  --output /tmp/manyforge-assistant-e2e/fail-closed.json
```

Pass signal: all `negativeCases` report `ok: true`.

### Cancel Proxy

Goal: the UI can request cancellation for a running assistant request and see a
structured terminal status.

Pass signal: `cancelProxy.ok: true`, the cancel response reports
`cancelled: true`, and the final request status is `cancelled` or another
terminal state if the model completed before cancellation arrived.

### Model Swap Bake-Off

Goal: compare candidate models under the same ManyForge workflow.

For each profile:

1. Stop vLLM and drop caches with the memory protocol.
2. Start the model with NemoClaw-Thor `./serving/start-model.sh <profile>`.
3. Run `./setup/configure-local-provider.sh <profile>`.
4. Run the primary qualification command above.
5. Record direct tool-call, query, draft-edit, golden-scene, and repair-turn
   behavior.

Current observations from May 1, 2026:

| Model | Result | Timings |
|---|---|---|
| `qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge` | Passed direct tool, query, draft edit; strong repair behavior | direct ~5s, query ~6s, draft ~18s |
| `nemotron3-nano-omni-30b-a3b-nvfp4` | Passed direct tool, query, draft edit at 32K no-thinking | direct ~9s, query ~4-5s, draft ~16-18s |
| `cosmos-reason2-8b` | Passed direct tool and query; weaker draft-edit repair | direct ~17s, query ~9s, failed draft repair in the 32K run |

## Future Recipes

Add these as the implementation grows:

- program-tree draft edits beyond scene resources,
- Isaac/ROS preview E2E checks,
- error-recovery mode sessions from `STOPPED`,
- visual/spatial reasoning checks routed to a VLM specialist,
- ManyMove-derived motion/scene workflows as they are reimplemented in
  ManyForge.

## Cleanup

```bash
pkill -f '[m]anyforge_assistant_bridge.bridge' 2>/dev/null || true
docker rm -f manyforge-e2e-composer 2>/dev/null || true
docker rm -f manyforge-e2e-vllm 2>/dev/null || true
sync && sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches'
free -g
```

Stop OpenShell/NemoClaw pieces with the NemoClaw-Thor runbook if needed.
