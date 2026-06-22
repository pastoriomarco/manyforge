# ManyForge Runtime Documentation Index

This index covers documentation that travels with the implementation
repository — install, operate, and reference. **Operators do not need any
other repo for runtime tasks.** Maintainers can also follow the
"Development / spec references" section at the bottom.

`README.md` remains the project entrypoint. For LLM agents,
[`../AGENTS.md`](../AGENTS.md) is the canonical landing page (covers
install, the three deployment modes, boundary rules, and the
branch/commit workflow).

## Operator runbooks (`docs/operations/`)

- [`operations/LAUNCHER_QUICKSTART.md`](operations/LAUNCHER_QUICKSTART.md)
  - Canonical guide for the unified launcher (`scripts/launch.sh`).
    Interactive 7-step wizard, scripted / non-interactive use for CI,
    `start` / `status` / `stop` / `restart` / `smoke` / `preflight`
    verbs, and the four-combination smoke-test matrix. **Start here
    when bringing the stack up.**

- [`operations/STACK_SETUP.md`](operations/STACK_SETUP.md)
  - Compact operational runbook for local bring-up, the four supported
    deployment modes (deterministic, ROS, direct LLM, NemoClaw/OpenClaw),
    primary script reference, and troubleshooting.

- [`operations/ASSISTANT_E2E_COOKBOOK.md`](operations/ASSISTANT_E2E_COOKBOOK.md)
  - End-to-end assistant lane recipes, known-good demo launch,
    model/profile smoke checks, and the chat-required-fields gotcha that
    silently falls back to the mock provider.

- [`operations/LANE_BRINGUP.md`](operations/LANE_BRINGUP.md)
  - Per-lane bring-up, operate, and live-monitor for all three assistant
    lanes (direct `:8100` / openclaw `:8200` / hermes `:8300`): start
    commands, health checks, teardown. Lane **dev/analysis deep-dives**
    (architecture, benchmarks, scorer-strictness analysis) live in the
    sibling `NemoClaw-Thor` repo — start at its `INDEX.md` →
    "The three assistant lanes".

- [`operations/ROS2_MEDKIT.md`](operations/ROS2_MEDKIT.md)
  - ros2_medkit integration runbook: install/build medkit, run the
    fault-manager + ManyForge adapter and the generic diagnostics bridge,
    test layers (hermetic vs real-lifecycle), production checklist and
    fail-closed semantics.

- [`operations/STOP_PROFILES.md`](operations/STOP_PROFILES.md)
  - Operator runbook for controlled-stop profiles: selectable modes,
    editable deployment fields, mandatory hard-stop floor, stop history,
    offline qualification, and live validation checklist.

- [`operations/ISAAC_SCENE_AUTHORING.md`](operations/ISAAC_SCENE_AUTHORING.md)
  - Operator runbook for exporting Isaac Sim scene meshes and loading
    them through Composer.

- [`operations/ADDING_A_ROBOT.md`](operations/ADDING_A_ROBOT.md)
  - Operator runbook for adding a new robot deployment: URDF/SRDF
    requirements, deployment-YAML schema (arm + gripper), capability
    adapter selection, smoke-test curls.

- [`operations/EXAMPLES_CATALOG.md`](operations/EXAMPLES_CATALOG.md)
  - Catalog of every `examples/*.program.yaml` × `*.deployment.yaml`
    pair, mapped to the launcher scenarios in `scripts/lib/scenarios.sh`.
    Use this to pick the right combination for a test or demo.

- [`operations/OBSERVABILITY.md`](operations/OBSERVABILITY.md)
  - Log file locations, observability REST endpoints (health, kernel
    events/metrics/snapshot, execution history, debug export, scene
    diagnostics), diagnostic scripts, env vars, and container names.

- [`operations/TROUBLESHOOTING.md`](operations/TROUBLESHOOTING.md)
  - Cross-cutting symptom → cause → diagnostic-step matrix.
    Detailed flows for collision rejection, motion interrupted,
    assistant mock-fallback, and ros2_control discovery failures.

## Reference contracts (`docs/reference/`)

These are auto-generated from code (FastAPI's `openapi()` output and the
FastMCP tool registry). Regenerate with
`scripts/regen-public-contracts.py`; CI guards staleness via
`scripts/ci/check-public-contracts-fresh.sh`.

- [`reference/REST_API.md`](reference/REST_API.md)
  - Composer REST surface (route table generated from the live
    OpenAPI). The live schema is also at `:9000/openapi.json` once
    Composer is running.

- [`reference/SCENE_AUTHORITY.md`](reference/SCENE_AUTHORITY.md)
  - Contract for live planning-scene writers: program scene nodes,
    explicit reset/load seed, lifecycle-safe operator edits, and
    configured pose-source feeds.

- [`reference/MCP_RUNTIME_CONTRACT.md`](reference/MCP_RUNTIME_CONTRACT.md)
  - MCP tool contract used by the OpenClaw assistant lane (generated
    from the `@mcp.tool()` decorators in
    `manyforge_composer/backend/mcp_server.py`).

- [`reference/ASSISTANT_PROVIDER_CONTRACT.md`](reference/ASSISTANT_PROVIDER_CONTRACT.md)
  - HTTP wire contract for the assistant-provider bridge service.

- [`reference/ROBOPLAN_ADAPTER_NOTES.md`](reference/ROBOPLAN_ADAPTER_NOTES.md)
  - Runtime notes for the RoboPlan adapter: projection rules,
    detached-static collision filter, support-contact handling, and
    temporary backend workarounds.

## Runtime template artifacts

These files are copied into OpenClaw/NemoClaw runtime sandboxes. They
are runtime payloads, not repo-level development instructions.

- [`../scripts/templates/AGENTS.md`](../scripts/templates/AGENTS.md)
- [`../scripts/templates/SOUL.md`](../scripts/templates/SOUL.md)
- [`../scripts/templates/skills/manyforge-monitor-execution/SKILL.md`](../scripts/templates/skills/manyforge-monitor-execution/SKILL.md)
- [`../scripts/templates/skills/manyforge-diagnose-failure/SKILL.md`](../scripts/templates/skills/manyforge-diagnose-failure/SKILL.md)
- [`../scripts/templates/skills/manyforge-intervene-recovery/SKILL.md`](../scripts/templates/skills/manyforge-intervene-recovery/SKILL.md)
- [`../scripts/templates/skills/manyforge-escalate-to-human/SKILL.md`](../scripts/templates/skills/manyforge-escalate-to-human/SKILL.md)
- [`../scripts/templates/skills/manyforge-delegated-recovery/SKILL.md`](../scripts/templates/skills/manyforge-delegated-recovery/SKILL.md)

## Development / spec references (require private `manyforge_specs` repo)

ManyForge development is spec-driven. The architecture specs, ADRs,
plans, open points, engineering references, and LLM-agent development
guidance live in a sibling **private** repository, `manyforge_specs`.

**Operators do not need any of this.** Runtime install, bring-up, and
operation are fully documented above and do not require
`manyforge_specs` to be on disk. The links below are dead unless you
are a maintainer with that repo cloned alongside `manyforge`.

Maintainers doing non-trivial behavior changes (kernel state machine,
adapter contracts, deployment schema, assistant authority, observability
surfaces, ADR-level architecture) need:

- `../../manyforge_specs/AGENTS.md`
- `../../manyforge_specs/docs/INDEX.md`
- `../../manyforge_specs/docs/agent-playbook.md`
- `../../manyforge_specs/docs/implementation/manyforge-implementation-workflow.md`
- `../../manyforge_specs/docs/cross-workspace-conventions.md`
- `../../manyforge_specs/docs/spec/`
- `../../manyforge_specs/docs/adr/`
- `../../manyforge_specs/docs/plans/`
- `../../manyforge_specs/docs/reference/`

If `../../manyforge_specs/` is not on disk, this code repository is still
fully operable — it is intentionally missing only the authoritative
development guidance required for spec-governed code changes.
