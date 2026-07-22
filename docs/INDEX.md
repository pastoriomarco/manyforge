# ManyForge Runtime Documentation Index

This index covers documentation that travels with the implementation
repository — install, operate, and reference. **Operators do not need any
other repo for runtime tasks.** Maintainers can also follow the
"Development / spec references" section at the bottom.

`README.md` remains the project entrypoint. For LLM agents,
[`../AGENTS.md`](../AGENTS.md) is the canonical landing page (covers install,
robot-execution and assistant-provider lane selection, boundary rules, and the
branch/commit workflow).

## Choose A Path

| Task                                        | Start here                                                                                                                          |
| ------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Bring up, stop, restart, or inspect a stack | [`operations/LAUNCHER_QUICKSTART.md`](operations/LAUNCHER_QUICKSTART.md)                                                            |
| Choose a bundled program/deployment         | [`operations/EXAMPLES_CATALOG.md`](operations/EXAMPLES_CATALOG.md)                                                                  |
| Diagnose a runtime symptom                  | [`operations/TROUBLESHOOTING.md`](operations/TROUBLESHOOTING.md), then [`operations/OBSERVABILITY.md`](operations/OBSERVABILITY.md) |
| Operate stop/recovery controls              | [`operations/STOP_PROFILES.md`](operations/STOP_PROFILES.md)                                                                        |
| Add a robot or trusted external node        | [`operations/ADDING_A_ROBOT.md`](operations/ADDING_A_ROBOT.md), [`operations/EXTERNAL_NODES.md`](operations/EXTERNAL_NODES.md)      |
| Understand ownership and data flow          | [`reference/ARCHITECTURE.md`](reference/ARCHITECTURE.md)                                                                            |
| Integrate an API/client                     | Generated REST/MCP/viewer contracts under [`reference/`](reference/)                                                                |

## Operator runbooks (`docs/operations/`)

- [`operations/LAUNCHER_QUICKSTART.md`](operations/LAUNCHER_QUICKSTART.md)

  - Canonical guide for the unified launcher (`scripts/launch.sh`).
    Interactive 7-step wizard, scripted / non-interactive use for CI,
    `start` / `status` / `stop` / `restart` / `smoke` / `preflight`
    verbs, and the four-combination smoke-test matrix. **Start here
    when bringing the stack up.**

- [`operations/STACK_SETUP.md`](operations/STACK_SETUP.md)

  - Compact operational runbook for native/ROS execution and the independent
    off/direct/OpenClaw/Hermes assistant selection, primary script reference,
    and troubleshooting.

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

- [`operations/REALTIME_RESOURCE_PROFILE.md`](operations/REALTIME_RESOURCE_PROFILE.md)

  - Opt-in Linux scheduler/affinity/memlock profile, JTC-first priority order,
    live `/proc` verification, and the boundary between process-level
    acceptance and full PREEMPT_RT/CPU-isolation qualification.

- [`operations/ISAAC_SCENE_AUTHORING.md`](operations/ISAAC_SCENE_AUTHORING.md)

  - Operator runbook for exporting Isaac Sim scene meshes and loading
    them through Composer.

- [`operations/ISAAC_UR10E_ROBOTIQ_GRIPPER.md`](operations/ISAAC_UR10E_ROBOTIQ_GRIPPER.md)

  - UR10e/Robotiq Isaac command authority, six-joint settled completion,
    readiness, measured-TCP binding, live smoke checks, and failure diagnosis.

- [`operations/ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md`](operations/ISAAC_LITE6_TROCAR_FOUNDATIONPOSE.md)

  - Multi-machine Lite6 trocar workflow with Isaac Sim, Isaac ROS 4.4
    YOLOv8/FoundationPose perception, ManyForge ROS control, DDS/QoS
    isolation, TF checks, and smoke qualification.

- [`../config/ros/isaac_lite6_trocar_foundationpose.env.sh`](../config/ros/isaac_lite6_trocar_foundationpose.env.sh)

  - Sourceable ROS/DDS environment profile for that workflow. Source it in
    the Isaac Sim laptop shell, the Isaac ROS perception container, and the
    ManyForge launcher shell so all ROS participants share the same domain,
    Fast DDS RMW, localhost discovery range, and static peers.

- [`operations/ADDING_A_ROBOT.md`](operations/ADDING_A_ROBOT.md)

  - Operator runbook for adding a new robot deployment: URDF/SRDF
    requirements, deployment-YAML schema (arm + gripper), capability
    adapter selection, smoke-test curls.

- [`operations/EXAMPLES_CATALOG.md`](operations/EXAMPLES_CATALOG.md)

  - Catalog of every `examples/*.program.yaml` × `*.deployment.yaml`
    pair, mapped to artifact-owned scenarios discovered from deployment
    compatibility metadata. Use this to pick or add a test/demo combination.

- [`operations/OBSERVABILITY.md`](operations/OBSERVABILITY.md)

  - Log file locations, observability REST endpoints (health, kernel
    events/metrics/snapshot, execution history, debug export, scene
    diagnostics), diagnostic scripts, env vars, and container names.

- [`operations/EXTERNAL_NODES.md`](operations/EXTERNAL_NODES.md)

  - Trusted external behavior-node installation, container-root configuration,
    validation CLI, deployment enablement and resource aliases, idle-only
    Composer reload, diagnostics, and supplier delivery checklist.

- [`operations/TROUBLESHOOTING.md`](operations/TROUBLESHOOTING.md)

  - Cross-cutting symptom → cause → diagnostic-step matrix.
    Detailed flows for collision rejection, motion interrupted,
    assistant mock-fallback, and ros2_control discovery failures.

## Architecture and integrator reference (`docs/reference/`)

- [`reference/ARCHITECTURE.md`](reference/ARCHITECTURE.md)

  - **Start here for the big picture.** Component map, authority
    boundaries (who owns terminal/stop/collision decisions), the three
    deployment lanes, execution lifecycle, and the observability model.
    Hand-maintained; keep in sync with material architecture changes.

- [`reference/SCENE_AUTHORITY.md`](reference/SCENE_AUTHORITY.md)

  - Contract for live planning-scene writers: program scene nodes,
    explicit reset/load seed, lifecycle-safe operator edits, and
    configured pose-source feeds.

- [`reference/ROBOPLAN_ADAPTER_NOTES.md`](reference/ROBOPLAN_ADAPTER_NOTES.md)

  - Runtime notes for the RoboPlan adapter: projection rules,
    detached-static collision filter, support-contact handling, and
    temporary backend workarounds.

### Generated public contracts

These files are generated from owning code/models and must not be edited by
hand. Regenerate with `scripts/regen-public-contracts.py`; CI guards freshness
with `scripts/ci/check-public-contracts-fresh.sh`.

- [`reference/REST_API.md`](reference/REST_API.md)

  - Composer REST surface generated from FastAPI/OpenAPI. The live schema is
    also at `:9000/openapi.json` while Composer is running.

- [`reference/MCP_RUNTIME_CONTRACT.md`](reference/MCP_RUNTIME_CONTRACT.md)

  - MCP tool contract generated from Composer's registered tool surface.

- [`reference/ASSISTANT_PROVIDER_CONTRACT.md`](reference/ASSISTANT_PROVIDER_CONTRACT.md)

  - HTTP wire contract for the assistant-provider bridge service.

- [`reference/VIEWER_DATA_PLANE_V1.schema.json`](reference/VIEWER_DATA_PLANE_V1.schema.json)
  and adjacent fixtures

  - Native-viewer manifest/frame/diagnostics/control wire contract.

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
