> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# ManyForge Stack Setup

Operational runbook for bringing up ManyForge locally, with or without
NemoClaw/OpenClaw. This file intentionally stays compact. It points to scripts
and live configuration sources instead of embedding their contents.

User-facing product overview belongs in [`../../README.md`](../../README.md).

## Scope

This runbook covers:

- ManyForge-only bring-up
- ManyForge plus direct local model endpoint
- ManyForge plus NemoClaw/OpenClaw integration
- the current recommended commands

It does not cover:

- detailed integration design rationale
- upstream platform gaps
- historical sprint logs

See also:

- [`../../../manyforge_specs/docs/plans/AI_ASSISTANT_INTEGRATION_PLAN.md`](../../../manyforge_specs/docs/plans/AI_ASSISTANT_INTEGRATION_PLAN.md)
- [`../../../manyforge_specs/docs/integration/PLATFORM_REQUIREMENTS.md`](../../../manyforge_specs/docs/integration/PLATFORM_REQUIREMENTS.md)
- [`../../../manyforge_specs/docs/plans/PRODUCT_DIRECTION_PLAN.md`](../../../manyforge_specs/docs/plans/PRODUCT_DIRECTION_PLAN.md)

## Current Supported Modes

### 1. Deterministic ManyForge only

No LLM, no NemoClaw/OpenClaw dependency.

Use when:

- validating tree execution
- debugging planning/execution
- avoiding any model/runtime integration variables

### 2. ManyForge plus direct local model endpoint

ManyForge talks directly to a local OpenAI-compatible server such as vLLM.

Use when:

- testing `query_model`
- validating bounded model interactions
- running the semantic selection demo on the current text-only baseline

### 3. ManyForge plus NemoClaw/OpenClaw

Two assistant-provider lanes coexist; **direct vLLM (`nemoclaw` provider) is
the demo default** as of 2026-05-06:

- **Direct vLLM lane (default — `ASSISTANT_PROVIDER=nemoclaw`)**: in-tree
  `manyforge_assistant_bridge` on `:8100` makes chat-completions calls
  against vLLM directly, runs the agent loop in-process. Sub-2 s warm
  latency, ~22 s/test p50 across the comparison matrix. Use this for
  local development, demos, and any single-tenant deployment where the
  user trusts the model.

- **OpenClaw lane (`ASSISTANT_PROVIDER=openclaw`)**: persistent OpenClaw
  gateway in the NemoClaw `my-assistant` sandbox; Composer's `openclaw`
  provider talks to the in-tree NemoClaw-Thor adapter at `:8200` which
  forwards to `/v1/chat/completions` on the gateway. The gateway runs
  the agent loop internally; ManyForge enforces every tool call via the
  mode-scoped MCP wrapper + `/api/assistant/bridge/tools/{toolId}`.
  Warm latency 5-15 s per turn but ~80 s/test p50 (3.6× direct).
  Recommended for shared/multi-tenant deployments where sandbox
  isolation + the bounded-autonomy audit envelope (assistantMode +
  catalogHash + requestId + principal) matter. Setup recipe:
  [`./ASSISTANT_E2E_COOKBOOK.md`](./ASSISTANT_E2E_COOKBOOK.md) →
  "Recipe: Known-Good Assistant Demo Launcher". Integration design:
  [`../../../../nemoclaw/src/NemoClaw-Thor/manyforge/docs/MANYFORGE-MCP-INTEGRATION.md`](../../../../nemoclaw/src/NemoClaw-Thor/manyforge/docs/MANYFORGE-MCP-INTEGRATION.md).

Both lanes preserve the same bounded-autonomy contract on the ManyForge
side. Switching between them is a single env var on
`scripts/demo-assistant-known-good.sh` (`ASSISTANT_PROVIDER=openclaw`).
The default switch is justified by the 2026-05-06 comparison matrix:
both paths solve the same 14/15 task set, but direct is consistently
3-11× faster per test. See
[`COMPARISON-2026-05-06-direct-vs-openclaw.md`](../../../../nemoclaw/src/NemoClaw-Thor/manyforge/docs/COMPARISON-2026-05-06-direct-vs-openclaw.md)
in NemoClaw-Thor for the per-test breakdown.

Use when:

- validating sandbox-to-ManyForge agent flows
- working on assistant, monitoring, or delegated-recovery integration

## Prerequisites

### Host

- Docker / Docker Compose
- local model server if testing LLM paths
- NemoClaw-Thor checkout if testing OpenClaw integration

### Repositories

- `manyforge`
- `manyforge_specs`
- `NemoClaw-Thor` when using the integrated stack

## Primary Scripts

### `scripts/demo-shape-pick.sh`

Purpose:

- start the semantic shape-pick demo
- rewrite the example deployment/program into temp artifacts for the chosen
  runtime options

Recommended baseline:

```bash
./scripts/demo-shape-pick.sh \
  --model-id Qwen3.5-9B-Claude-Distilled-NVFP4 \
  --selection-image-mode text \
  --skip-bootstrap
```

Notes:

- `text` is the current supported baseline
- `inline` and `http` image modes are experimental placeholders only

### `scripts/manyforge-stack.sh`

Purpose:

- start/stop the local ManyForge stack
- optionally wire it into NemoClaw/OpenClaw workflows

Typical usage:

```bash
./scripts/manyforge-stack.sh up
./scripts/manyforge-stack.sh up --with-nemoclaw
./scripts/manyforge-stack.sh status
./scripts/manyforge-stack.sh down
```

### `scripts/configure-manyforge-provider.sh`

Purpose:

- configure the current mux/plugin-based ManyForge provider path for
  NemoClaw/OpenClaw

Use this only for the current sandbox integration baseline. It is not the final
transport design.

### `scripts/manyforge-session-start.sh`

Purpose:

- restore sandbox reference materials and contracts for a new session
- upload the engineering reference and specs into the sandbox

## Recommended Bring-Up Sequences

### ManyForge only

```bash
docker compose up --build
```

Then use:

- Composer API on port `9000`
- HMI/Viser on port `8081`

### Direct local model testing

1. Start the model server separately.
2. Point ManyForge deployment/model alias at that endpoint.
3. Prefer the semantic demo in text-only mode first.

### NemoClaw/OpenClaw integration

1. Bring up the local provider/model path in `NemoClaw-Thor`.
2. Run `scripts/configure-manyforge-provider.sh`.
3. Use `scripts/manyforge-session-start.sh` to seed sandbox docs/contracts.
4. Validate tool discovery and health before testing agent behavior.

## Operational Notes

### Temp demo artifacts

The semantic demo launcher creates temp files such as:

- `.manyforge-demo-shape-pick.<id>.program.yaml`
- `.manyforge-demo-shape-pick.<id>.deployment.yaml`

These are per-run rewritten artifacts. They are not source docs and should not
be treated as durable repository content.

### Model aliasing

Composer nodes select deployment aliases, not raw model IDs. In single-alias
cases the UI should show resolved read-only info, not fake choice.

### Blackboard inspection baseline

For current selection/model/planning flows:

- declared blackboard artifacts are authored through `namespace_ref`,
  `key_ref`, and `field_ref`
- runtime namespace outputs appear as one root bundle per namespace
- when inspecting `/api/execution/blackboard`, expect bundle-shaped values with
  `meta` and `value`, not the old scattered `response/status/fields/...` form

### RoboPlan planner trace logging

Native RoboPlan planner traces are disabled by default to keep normal demo and
qualification logs readable. Enable them only while diagnosing planner behavior:

```bash
MANYFORGE_ROBOPLAN_PLANNER_DEBUG=1 ./scripts/demo-ur10e-robotiq-pick-place.sh
```

Accepted disabled values are unset, `0`, `false`, and `FALSE`. Any other
non-empty value enables `[PLANNER]` stderr traces from the native RoboPlan
backend.

### Runtime image modes

Current status:

- `text`: supported baseline
- `inline`: experimental
- `http`: experimental and topology-sensitive

Do not treat browser-backed image capture as a production path.

## Troubleshooting

### Tree does not progress after `Run`

Check:

- cycle state via `GET /api/cycle/status`
- execution digest via `GET /api/execution/digest`
- event history via `GET /api/execution/history`

If testing image-enabled demos:

- switch back to `--selection-image-mode text`
- browser-backed image capture is still experimental

### Model selection appears ignored

Check deployment aliases rather than raw model names. The node chooses the
alias; the deployment maps that alias to the actual provider/model endpoint.

### Sandbox agent cannot reach ManyForge cleanly

That is expected on the current platform baseline. See:

- [`../../../manyforge_specs/docs/plans/AI_ASSISTANT_INTEGRATION_PLAN.md`](../../../manyforge_specs/docs/plans/AI_ASSISTANT_INTEGRATION_PLAN.md)
- [`../../../manyforge_specs/docs/integration/PLATFORM_REQUIREMENTS.md`](../../../manyforge_specs/docs/integration/PLATFORM_REQUIREMENTS.md)

## Keep This File Small

When scripts change:

- update the command examples and scope notes here
- do not paste entire script bodies into this document
- treat the script files as the source of truth
