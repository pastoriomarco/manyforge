# ManyForge Runtime Documentation Index

This index covers documentation that travels with the implementation
repository: current structural inspection plus historical install, operation,
and integration references. **No product runtime task is currently
available.** When a lane is restored, its public operator documentation must
remain self-contained here. Maintainers can also follow the "Development /
spec references" section at the bottom.

`README.md` remains the project entrypoint. For LLM agents,
[`../AGENTS.md`](../AGENTS.md) is the canonical landing page (covers the
current outage, historical lane selection, boundary rules, and the
branch/commit workflow).

> **Current `dev` outage:** the composable-cell destructive migration has
> removed the legacy node factory and runtime resolver. Catalog inspection is
> available; Composer program execution and the native, Isaac, and real-robot
> product lanes are not. H0 restores headless resolution, N restores native
> execution, and I/R restore Isaac/real operation. Operator runbooks below
> remain baseline/reference material until their owning restoration gate.

## Choose A Path

| Task                                        | Start here                                                                                                                          |
| ------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| Start the structural Composer shell         | `../scripts/compose-dev.sh`; catalog/files/URDF/health only                                                                          |
| Study former stack workflows                | [`operations/LAUNCHER_QUICKSTART.md`](operations/LAUNCHER_QUICKSTART.md), marked historical                                           |
| Study historical runtime diagnosis          | [`operations/TROUBLESHOOTING.md`](operations/TROUBLESHOOTING.md) and [`operations/OBSERVABILITY.md`](operations/OBSERVABILITY.md)     |
| Study historical stop/recovery controls     | [`operations/STOP_PROFILES.md`](operations/STOP_PROFILES.md)                                                                         |
| Study the historical robot-import workflow | [`operations/ADDING_A_ROBOT.md`](operations/ADDING_A_ROBOT.md); v0 migration/reference input                                      |
| Study historical physical-Lite6 qualification | [`operations/LITE6_LIVE_QUALIFICATION.md`](operations/LITE6_LIVE_QUALIFICATION.md); unavailable until R/I10                        |
| Understand ownership and data flow          | [`reference/ARCHITECTURE.md`](reference/ARCHITECTURE.md)                                                                            |
| Integrate the current structural API        | Generated [`reference/REST_API.md`](reference/REST_API.md); former clients use the historical interaction index                       |

## Historical operator runbooks (`docs/operations/`)

These runbooks preserve workflow and qualification intent for later
restoration. They do not describe currently available products, and their
commands must not be run or cited as current lane evidence.

- [`operations/LAUNCHER_QUICKSTART.md`](operations/LAUNCHER_QUICKSTART.md)

  - Canonical guide for the unified launcher (`scripts/launch.sh`).
    Interactive 7-step wizard, scripted / non-interactive use for CI,
    `start` / `status` / `stop` / `restart` / `smoke` / `preflight`
    verbs, and the four-combination smoke-test matrix. This was the historical
    stack bring-up entrypoint; the current launcher is intentionally gated.

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

  - Historical ros2_medkit restoration runbook: preserved fault/healing
    behavior, former adapter and diagnostics-bridge recipes, and the I5/H0
    gate that must provide their new headless diagnostic source.

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

- [`operations/LITE6_LIVE_QUALIFICATION.md`](operations/LITE6_LIVE_QUALIFICATION.md)

  - Staged real-Lite6 qualification: exact production JTC binding, offline
    fake-hardware HIL, bounded read-only evidence, live lifecycle gates, and
    the later physical-motion boundary.

- [`../config/ros/isaac_lite6_trocar_foundationpose.env.sh`](../config/ros/isaac_lite6_trocar_foundationpose.env.sh)

  - Sourceable ROS/DDS environment profile from that workflow. The captured
    procedure sourced it in the Isaac Sim laptop shell, Isaac ROS perception
    container, and ManyForge launcher shell so all participants shared the
    same domain, Fast DDS RMW, localhost discovery range, and static peers.

- [`operations/ADDING_A_ROBOT.md`](operations/ADDING_A_ROBOT.md)

  - Historical v0 robot-import workflow and bounded migration reference:
    URDF/SRDF requirements, arm/gripper intent, adapter selection, and smoke
    acceptance cases. It is not the current artifact schema or runnable path.

- [`operations/EXAMPLES_CATALOG.md`](operations/EXAMPLES_CATALOG.md)

  - Migration and acceptance inventory of every captured
    `examples/*.program.yaml` × `*.deployment.yaml` pair, mapped to its
    historical scenario. It is not a source of runnable current combinations;
    each artifact must be ported through its named compiler and lane gate.

- [`operations/OBSERVABILITY.md`](operations/OBSERVABILITY.md)

  - Log file locations, observability REST endpoints (health, kernel
    events/metrics/snapshot, execution history, debug export, scene
    diagnostics), diagnostic scripts, env vars, and container names.

- [`operations/TROUBLESHOOTING.md`](operations/TROUBLESHOOTING.md)

  - Cross-cutting symptom → cause → diagnostic-step matrix.
    Detailed flows for collision rejection, motion interrupted,
    assistant mock-fallback, and ros2_control discovery failures.

## Architecture and integrator reference (`docs/reference/`)

- [`reference/ARCHITECTURE.md`](reference/ARCHITECTURE.md)

  - **Start here for the big picture.** Current cutover boundary plus the
    captured component map, authority boundaries, three deployment lanes,
    execution lifecycle, and observability model retained as restoration
    input. Hand-maintained; keep in sync with material architecture changes.

- [`reference/SCENE_AUTHORITY.md`](reference/SCENE_AUTHORITY.md)

  - Captured contract for live planning-scene writers and the current
    canonical-empty boundary. Its program nodes, reset/load seed, operator
    edits, and pose-source feeds are unavailable restoration requirements.

- [`reference/ROBOPLAN_ADAPTER_NOTES.md`](reference/ROBOPLAN_ADAPTER_NOTES.md)

  - Captured runtime notes for the RoboPlan adapter plus the current retained
    offline boundary: projection rules, detached-static collision filter,
    support-contact handling, and temporary backend workarounds are
    restoration inputs rather than a live adapter claim.

### Current generated public contract

These files are generated from owning code/models and must not be edited by
hand. Regenerate the retained surface with
`scripts/regen-public-contracts.py structural-rest`; CI guards freshness with
`scripts/ci/check-public-contracts-fresh.sh structural-rest`. The historical
no-argument aggregate forms are typed unavailable rather than silently
checking only this reduced projection.

- [`reference/REST_API.md`](reference/REST_API.md)

  - Composer REST surface generated from FastAPI/OpenAPI. The live schema is
    also at `:9000/openapi.json` while Composer is running.

### Historical Composer/client interaction references

- [`reference/HISTORICAL_COMPOSER_INTERACTION_REFERENCE.md`](reference/HISTORICAL_COMPOSER_INTERACTION_REFERENCE.md)
  records the exact full-Composer source tree and reimplementation rule.
- [`reference/MCP_RUNTIME_CONTRACT.md`](reference/MCP_RUNTIME_CONTRACT.md) and
  [`reference/ASSISTANT_PROVIDER_CONTRACT.md`](reference/ASSISTANT_PROVIDER_CONTRACT.md)
  are non-operative historical contracts. They are not generated from or
  exposed by the current shell.
- [`reference/VIEWER_DATA_PLANE_V1.schema.json`](reference/VIEWER_DATA_PLANE_V1.schema.json)
  and its
  [`fixture`](reference/VIEWER_DATA_PLANE_V1.fixture.json), plus the historical
  [`qualification artifact`](reference/VIEWER_QUALIFICATION_ARTIFACT_V1.schema.json)
  and
  [`qualification matrix`](reference/VIEWER_QUALIFICATION_MATRIX_V1.schema.json),
  are non-operative wire/acceptance inputs. They do not describe the current
  structural shell and must be translated rather than reactivated. Their
  generated frontend types remain absent; the full interaction source is
  pinned at `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`.

## Historical runtime-agent interaction artifacts

These files are retained with non-operative banners as future interaction and
safety-design inputs. Their former frontmatter is deliberately disabled; they
must not be copied into a runtime sandbox during the cutover.

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

**Operators do not need the private specifications to inspect the currently
available structural shell or the retained historical workflows.** Product
runtime install, bring-up, and operation remain unavailable during the
cutover. The links below are dead unless you are a maintainer with
`manyforge_specs` cloned alongside `manyforge`.

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

If `../../manyforge_specs/` is not on disk, this repository still contains its
public structural-shell documentation and historical operator references. It
does not make the intentionally removed product runtime operable.
