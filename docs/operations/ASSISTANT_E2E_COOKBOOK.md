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

- ManyForge repo: `/home/tndlux/workspaces/dev_ws/src/manyforge`
- NemoClaw-Thor repo:
  `/home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor`
- Docker image for the ManyForge dev lane is built: `manyforge-dev:latest`
- NemoClaw-Thor vLLM image/profile support is already set up.
- Thor memory protocol is followed before each model switch.

Current primary candidate profiles:

```text
nemotron3-nano-omni-30b-a3b-nvfp4  # current known-good single-model demo default
qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge
cosmos-reason2-8b
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
cd /home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor

THOR_DETACH=1 \
THOR_CONTAINER_NAME=manyforge-e2e-vllm \
./start-model.sh nemotron3-nano-omni-30b-a3b-nvfp4
```

For Qwen3.6 or Cosmos comparison runs, replace the profile with:

```text
qwen3.6-35b-a3b-nvfp4-tq-mtp-manyforge
cosmos-reason2-8b
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
cd /home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor

./configure-local-provider.sh nemotron3-nano-omni-30b-a3b-nvfp4
./status.sh
```

### 4. Start Composer With The Live Assistant Provider And HMI

Use host networking so Composer can reach the host bridge on `127.0.0.1:8100`.
Start Composer with `--hmi-port 8081` for UI demos; otherwise the embedded
Viser panel correctly reports that the HMI is unavailable.

```bash
cd /home/tndlux/workspaces/dev_ws/src/manyforge

docker rm -f manyforge-e2e-composer 2>/dev/null || true

docker run -d --rm \
  --name manyforge-e2e-composer \
  --network host \
  -v /home/tndlux/workspaces/dev_ws/src/manyforge:/workspace \
  -v manyforge_build-cache:/tmp/manyforge-build \
  -w /workspace \
  -e MANYFORGE_ASSISTANT_PROVIDER=nemoclaw \
  -e MANYFORGE_ASSISTANT_ENDPOINT_URL=http://127.0.0.1:8100/v1/manyforge/assistant \
  -e MANYFORGE_ASSISTANT_TIMEOUT_S=180 \
  manyforge-dev:latest \
  bash -lc 'python -m manyforge_composer \
    --catalog-path /workspace/manyforge_behavior/resources/node_catalog.yaml \
    --host 127.0.0.1 \
    --port 9000 \
    --hmi-port 8081 \
    --assistant-provider nemoclaw \
    --assistant-endpoint http://127.0.0.1:8100/v1/manyforge/assistant \
    --assistant-timeout-s 180'
```

Verify:

```bash
curl -s http://127.0.0.1:9000/api/infra/status | python3 -m json.tool
```

Expected status after loading a program in the UI or through the smoke runner:

```text
composer.status = healthy
composer.cycle_state = idle
hmi.status = healthy
hmi.port = 8081
deployment.status = loaded
```

### 5. Install Bridge Dependencies

```bash
cd /home/tndlux/workspaces/dev_ws/src/manyforge

python3 -m venv manyforge_assistant_bridge/.venv
manyforge_assistant_bridge/.venv/bin/pip install -r manyforge_assistant_bridge/requirements.txt
```

## Recipe: Manual Bridge Start

Use this when you want to interact with Composer manually while keeping the
bridge process visible in a terminal.

```bash
cd /home/tndlux/workspaces/dev_ws/src/manyforge

BRIDGE_HOST=127.0.0.1 \
BRIDGE_PORT=8100 \
BRIDGE_UPSTREAM_BASE_URL=http://127.0.0.1:8000/v1 \
BRIDGE_MANYFORGE_BASE_URL=http://127.0.0.1:9000 \
BRIDGE_MODEL=nemotron3-nano-omni-30b-a3b-nvfp4 \
BRIDGE_REQUEST_TIMEOUT_S=180 \
BRIDGE_MAX_WALL_TIME_S=180 \
BRIDGE_MAX_TURNS=4 \
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
defaults to the NVIDIA Nemotron 3 Nano Omni profile because it is the current
preferred single-model candidate for agentic workflows and leaves room for
future text, vision, and audio input through one endpoint.

```bash
cd /home/tndlux/workspaces/dev_ws/src/manyforge

./scripts/demo-assistant-known-good.sh preflight
./scripts/demo-assistant-known-good.sh start
```

What it does:

1. stops stale `manyforge-e2e-vllm` state if needed and runs the Thor memory
   protocol before model launch,
2. starts `nemotron3-nano-omni-30b-a3b-nvfp4` through NemoClaw-Thor
   `start-model.sh`,
3. runs `configure-local-provider.sh` for the same profile,
4. starts Composer on `http://127.0.0.1:9000` with HMI/Viser on
   `http://127.0.0.1:8081`,
5. starts the ManyForge assistant bridge on `http://127.0.0.1:8100`,
6. loads `pick_and_place.program.yaml` with
   `assistant_modes_scene_authoring.deployment.yaml`.

The preflight refuses to start if ports `8000`, `8080`, `8081`, `8100`, or
`9000` are already owned by unmanaged processes. This prevents accidental
duplicate Composer, HMI, bridge, OpenShell, or vLLM instances. It reuses only
the known demo-owned containers/PID file and otherwise asks you to stop the
conflicting service explicitly.

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

Stop and free memory:

```bash
./scripts/demo-assistant-known-good.sh stop
```

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

MODEL_PROFILE=cosmos-reason2-8b \
  ./scripts/demo-assistant-known-good.sh restart
```

## Recipe: Manual Composer UI Demo

Use this after the base boot and manual bridge start when checking the actual
operator workflow.

1. Open `http://127.0.0.1:9000`.
2. Load `/workspace/examples/pick_and_place.program.yaml`.
3. Load `/workspace/examples/assistant_modes_scene_authoring.deployment.yaml`.
4. Confirm the Viser panel shows the robot/scene at `http://127.0.0.1:8081`.
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
   assistant transcript attributes the completed tool call, and Viser reflects
   the draft scene after Composer refreshes. There is no separate LLM approval
   layer; use the normal Composer apply/save/reload controls to keep or reject
   the change.

If the Viser panel says HMI is unavailable, Composer was not launched with
`--hmi-port 8081` or no program has been loaded yet. Restart Composer with the
base recipe command above, then reload the program/deployment.

## Recipe: Automated Assistant E2E Checks

The reusable runner starts and stops the bridge for each sampling profile. It
loads the assistant-mode deployment and a baseline program, then exercises:

- direct vLLM tool-call probing,
- query-mode `program.read`,
- operator-facing Composer assistant flow with provider discovery, HMI status,
  draft mutation, request status, diagnostics, and discard,
- composer-assistant `scene.draft.add_object`,
- golden-scene `scene.draft.upsert_objects` with expected object IDs, shapes,
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
cd /home/tndlux/workspaces/dev_ws/src/manyforge

./scripts/smoke-assistant-e2e.py \
  --profile omni-instruct-no-think-32768 \
  --require-hmi \
  --strict \
  --output /tmp/manyforge-assistant-e2e/assistant-primary.json
```

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
| `omni-vendor-tool` | NVIDIA tool-call sampling: `temperature=0.6`, `top_p=0.95`, `max_tokens=512`, thinking unset |
| `omni-vendor-tool-no-think` | Vendor sampling with `enable_thinking=false`, `max_tokens=512` |
| `omni-vendor-tool-no-think-32768` | Vendor sampling with `enable_thinking=false`, `max_tokens=32768` |
| `omni-instruct-no-think` | Instruct sampling: `temperature=0.2`, `top_k=1`, `max_tokens=512`, `enable_thinking=false` |
| `omni-instruct-no-think-128` | Output-budget sweep with `max_tokens=128` |
| `omni-instruct-no-think-256` | Output-budget sweep with `max_tokens=256` |
| `omni-instruct-no-think-1024` | Output-budget sweep with `max_tokens=1024` |
| `omni-instruct-no-think-32768` | Current realistic assistant profile: `max_tokens=32768`, `enable_thinking=false` |
| `omni-deterministic-no-think` | Deterministic non-thinking comparison |

## E2E Recipes

### Read-Only Query

Goal: the assistant reads current Composer state without mutating the draft.

Command:

```bash
./scripts/smoke-assistant-e2e.py \
  --profile omni-instruct-no-think-32768 \
  --skip-negative \
  --output /tmp/manyforge-assistant-e2e/query-and-draft.json
```

Pass signal: the `queryReadOnly` result contains a completed `program.read`
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
  --profile omni-instruct-no-think-32768 \
  --require-hmi \
  --skip-negative \
  --output /tmp/manyforge-assistant-e2e/operator-flow.json
```

Pass signal: `operatorComposerFlow.ok: true`, `providerCatalog.configuredProviderId`
matches the configured provider, `hmi.status: healthy`, exactly one
`scene.draft.upsert_objects` call completes, no tool call is outside
`composer-assistant`, the draft program contains the expected object before
reload, HMI diagnostics match when available, request status is `completed`,
and reload/discard removes the assistant-authored object.

### Golden Scene Upsert

Goal: the assistant reproduces a small expected scene from text by calling
`scene.draft.upsert_objects` once. This verifies more than transport: it checks
that the resulting draft contains the expected object IDs, shape types,
dimensions/radius, frame, and positions.

Pass signal: `goldenSceneUpsert.ok: true`, exactly one completed
`scene.draft.upsert_objects` call, no `validationErrors`, both expected objects
appear in `matchedExpectedObjects`, `actualUpsertObjectIds` has no extras, and
reload/discard removes the assistant-authored draft objects. The validator checks
tool-call arguments and resulting draft state for object IDs, shape
type/dimensions/radius, frame, position, and orientation. It also checks the
live `/api/program` draft, request-status metadata, and that the assistant text
mentions the expected object IDs.

### Golden Tree Insert

Goal: the assistant inserts a precise behavior-tree node into the Composer draft
from text by calling `tree.draft.insert_node` once.

Pass signal: `goldenTreeInsert.ok: true`, exactly one completed
`tree.draft.insert_node` call, no `validationErrors`, and reload/discard removes
the assistant-authored node. The validator checks parent, insertion index, node
ID/name, node parameters, resulting draft-tree placement in the tool response
and live `/api/program`, request-status metadata, and assistant text mention of
the inserted node name.

### Golden Parameter Upsert

Goal: the assistant inserts a precise program parameter into the Composer draft
from text by calling `program.draft.upsert_parameters` once.

Pass signal: `goldenParameterUpsert.ok: true`, exactly one completed
`program.draft.upsert_parameters` call, no `validationErrors`, and
reload/discard removes the assistant-authored parameter. The validator checks
tool-call arguments, resulting draft parameter metadata in the tool response
and live `/api/program`, request-status metadata, and assistant text mention of
the parameter name.

### Golden Blackboard Upsert

Goal: the assistant inserts a precise blackboard namespace and key into the
Composer draft from text by calling `blackboard.draft.upsert_namespaces` and
`blackboard.draft.upsert_keys`.

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
  --profile omni-instruct-no-think-32768 \
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
2. Start the model with NemoClaw-Thor `./start-model.sh <profile>`.
3. Run `./configure-local-provider.sh <profile>`.
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
