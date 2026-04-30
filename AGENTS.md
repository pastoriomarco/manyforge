# ManyForge Agent Instructions

This repository is the ManyForge implementation. Automated agents working here
must keep it aligned with the sibling ManyForge specifications repository.

## Authority

The canonical architecture, behavior, artifact, and testing rules live in:

- `../manyforge_specs/AGENTS.md`
- `../manyforge_specs/docs/adr/`
- `../manyforge_specs/docs/spec/`
- `../manyforge_specs/docs/testing/`

If this implementation repository conflicts with `manyforge_specs`, the specs
repository wins unless the user explicitly asks to change the specs.

## Precedence

When instructions conflict, follow this order:

1. System, developer, and user instructions.
1. `../manyforge_specs/docs/adr/`
1. `../manyforge_specs/AGENTS.md`
1. `../manyforge_specs/docs/spec/`
1. This file.
1. ManyForge implementation docs under `docs/`.
1. Tracking/planning docs under `docs/plans/` and other non-normative notes.

## Mandatory Drift Handling

Before substantial code changes:

- identify the relevant specs and ADRs,
- compare the implementation against those documents,
- call out any drift with concrete file paths.

If code and specs disagree:

- do not silently follow the code,
- state the conflict clearly,
- state which spec or ADR wins by precedence,
- fix the implementation or explicitly defer the fix with rationale.

Repeat the drift check before finishing when the task changes architecture,
runtime semantics, artifact schemas, deployment behavior, or quality gates.

When a change affects downstream agent-facing runtime behavior, also check the
runtime/sandbox agent docs. Changes to MCP tools, tool descriptions, lifecycle
states, kernel terminal semantics, runtime overrides, interventions, session
commands, deployment/capability outputs, or OpenClaw plugin packaging must
update or explicitly verify:

- `scripts/templates/AGENTS.md`
- `scripts/templates/SOUL.md`
- relevant files under `scripts/templates/skills/`
- `docs/reference/MCP_SCHEMA.md`

## External Model Profile References

NemoClaw-Thor owns deployment-environment model-serving profile selection for
NemoClaw/OpenClaw-backed ManyForge assistant and runtime model endpoints. It
does not override `manyforge_specs`, but it is the source to check before
changing committed example model IDs, demo defaults, assistant endpoint
guidance, or model capability assumptions.

Primary reference:

- `/home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor/MANYFORGE-ASSISTANT-DEPLOYMENT-PLAN.md`

Profile implementation references:

- `/home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor/lib/config.sh`
- `/home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor/lib/launch.sh`
- `/home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor/start-duo.sh`

When changing `model_endpoints`, assistant provider defaults, demo scripts, or
plans that discuss Cosmos, Qwen3.6, Omni, or later profiles:

- check the NemoClaw-Thor plan and profile scripts first,
- keep ManyForge program artifacts model-agnostic through role aliases,
- keep committed deployment profile registries concrete and deployments fully
  validated after profile resolution,
- prefer deployment-environment overlays or registries for site-specific
  profile selection,
- do not introduce arbitrary environment interpolation into normative
  deployment artifacts, except for secret indirection such as `api_key_env`.

## Cross-Workspace Coordination

This repository is one of three loosely-coupled workspaces that
collaborate on the ManyForge assistant pipeline:

- `../manyforge_specs/` — normative architectural specs (this is the
  spec layer; precedence is already defined above).
- this repository (`manyforge`) — the implementation: composer,
  behavior runtime, assistant-provider HTTP contract code, and the
  `manyforge_assistant_bridge/` runtime service.
- `/home/tndlux/workspaces/nemoclaw/src/NemoClaw-Thor/` — deployment
  helper for Jetson AGX Thor: vLLM container builds, model profiles,
  launch scripts, OpenShell sandbox onboarding.

The orientation map for these three workspaces — authority by
concern, information flow, the cross-workspace change protocol, and
the read-before-claim rule below — lives in
`../manyforge_specs/docs/cross-workspace-conventions.md`. Read it
before working on anything that crosses workspace boundaries (the
assistant-provider envelope, the bridge service, deployment-artifact
fields shared with NemoClaw-Thor, model-serving profile references).

### Read-before-claim rule

Before implementing or claiming scope for anything that crosses a
workspace boundary, read the relevant source code in the other
workspace, not just its docs. Wire shapes, validation rules, and
runtime semantics are in code; documentation summarises them but can
lag. When in doubt, `grep` the code in the other workspace before
making the claim or writing the implementation.

This rule applies in both directions — when this repo modifies
something that NemoClaw-Thor consumes (for example, the wire shape
the bridge accepts), the relevant NemoClaw-Thor files
(`MANYFORGE-PROFILE-CALIBRATION.md`,
`NEMOCLAW-OPENCLAW-WORKFLOW.md`, `lib/config.sh`, `lib/launch.sh`)
should be read to confirm the change is consistent with how
NemoClaw-Thor exposes the deployment.

## Architecture Rules

- `manyforge_core` remains ROS-free. It owns DTOs, policies, ports, and the
  Execution Kernel.
- ROS code stays in ROS adapter/wrapper packages. ROS nodes own wiring and
  lifecycle only.
- Behavior trees are orchestration only. They must not own safety-critical
  stop, pause, resume, reset, cache, or collision semantics.
- The Execution Kernel owns stop/pause/resume/reset, validation gates,
  trajectory cache behavior, collision monitoring, and execution state.
- The orchestrator/runtime, not authored behavior-tree nodes, owns successful
  kernel terminal acknowledgement. `AcknowledgeCompleted(execution_id)` must be
  bound to the exact completed execution ID per spec 230 section 3.7.
- Backend selection and adapter configuration belong in deployment artifacts or
  launch configuration, not program artifacts.
- Program artifacts must remain backend-portable for their declared
  `program_profile`.
- Supported backend claims require explicit capability declarations and
  contract tests. Do not add partial support modes that require users to
  rewrite programs per backend.
- Runtime overrides, interventions, assistant proposals, and scene edits must
  remain explicit, auditable, and bounded by review/apply workflows where
  required.
- UI, HMI, assistant, and browser-rendered data must not become hidden sources
  of runtime truth when a backend-authoritative query path is required.

## Test Workflow

Start every non-trivial change with a test plan. Cover:

- unit or component tests for local logic,
- integration tests for cross-package behavior,
- Docker or ROS tests when touching containers, ROS, Isaac, simulator, or
  deployment paths,
- explicit deferred coverage if a check cannot be run locally.

Tests are run in the appropriate Docker lane, not directly on the host:

- core, RoboPlan, pybind11, Python, Composer, and general integration tests run
  in the ROS-free `dev` service,
- ROS adapter/wrapper tests run in the `ros-jazzy` service,
- full ManyForge + ROS integration tests run in the heavier `ros-jazzy-full`
  service.

CI is an architectural quality-gate requirement in `manyforge_specs`, but this
repository currently has no `.github/workflows/ci.yml`. Until CI is added here,
local Docker lane validation is the qualification surface agents must run and
report.

Use targeted checks in the correct container while iterating, then broaden
before handoff. Common commands from the repository root:

```bash
git diff --check
```

```bash
docker compose run --rm dev pytest <targeted-python-tests> -q
```

```bash
docker compose run --rm dev ./scripts/test-all.sh cpp
docker compose run --rm dev ./scripts/test-all.sh roboplan
docker compose run --rm dev ./scripts/test-all.sh bindings
docker compose run --rm dev ./scripts/test-all.sh python
docker compose run --rm dev ./scripts/test-all.sh integration
docker compose run --rm dev ./scripts/test-all.sh composer
```

```bash
docker compose run --rm dev ./scripts/test-all.sh all
```

```bash
docker compose --profile ros run --rm ros-jazzy ./scripts/test-ros.sh
```

```bash
docker compose --profile ros-full run --rm ros-jazzy-full ./scripts/test-ros-full.sh
```

For C++ packages touched outside the aggregate runner, still run the manual
build inside the `dev` container:

```bash
docker compose run --rm dev bash -lc 'cmake -S manyforge_core -B /tmp/manyforge-build/core-manual -DBUILD_TESTING=ON && cmake --build /tmp/manyforge-build/core-manual && ctest --test-dir /tmp/manyforge-build/core-manual --output-on-failure'
```

```bash
docker compose run --rm dev bash -lc 'cmake -S manyforge_roboplan -B /tmp/manyforge-build/roboplan-manual -DBUILD_TESTING=ON && cmake --build /tmp/manyforge-build/roboplan-manual && ctest --test-dir /tmp/manyforge-build/roboplan-manual --output-on-failure'
```

Host Python may not have the full Composer/backend dependency set and host ROS
must not be treated as the qualification environment. If Docker is unavailable,
state that explicitly and report the validation gap.

## Industrial Code Quality

- Prefer explicit validation errors over permissive fallbacks for artifacts,
  capability declarations, backend compatibility, and runtime authority.
- Do not add sleeps or timing hacks in core/kernel tests. Use injected
  `IClock` and `IScheduler` with `FakeClock` or `ManualScheduler`.
- Do not introduce blocking I/O on critical stop/control paths.
- Preserve structured error taxonomy, traceability, and event/audit records.
- Keep scene ownership backend/adaptor-owned via `IPlanningContext`.
- Validate cached trajectories against the current backend-owned scene before
  reuse.
- Enforce deployment-level and per-move motion limits at the kernel validation
  boundary.
- Treat assistant/model output as advisory unless a spec explicitly grants more
  authority. Assistant/provider code must not directly mutate Composer,
  runtime, or program state.
- Avoid unrelated refactors, format churn, or generated-file noise.

## Definition Of Done

A change is ready for review only when:

- relevant specs and ADRs were checked,
- implementation/spec drift is fixed or clearly reported,
- relevant tests were added or updated,
- relevant tests were run, or skipped with a concrete reason,
- `git diff --check` passes,
- new interfaces, artifact behavior, or workflows are documented or covered by
  tests,
- remaining risks are stated in the handoff.

If `pre-commit run --all-files` is available in the environment, run it before
handoff. If it is unavailable, say so and report the checks that were run
instead.
