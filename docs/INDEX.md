# ManyForge Documentation Index

Internal documentation entrypoint for the ManyForge repository.

Use this file to understand what each Markdown document is for and where to go
next. `README.md` remains the user-facing entrypoint; this index is for
contributors, operators, and agent/tooling workflows that need the internal doc
map.

## User Entry

- [`../README.md`](../README.md)
  - User-facing project entrypoint.
  - What ManyForge is, how to run it at a high level, and where the main
    surfaces live.

## Live Internal Docs

- [`guides/LLM_GUIDE.md`](guides/LLM_GUIDE.md)
  - Agent/operator guide for using the ManyForge API and composing programs.
  - Practical patterns, rules, and endpoint quick reference.

- [`guides/INTERRUPT_DEMO.md`](guides/INTERRUPT_DEMO.md)
  - Operator guide for the interrupt-focused pick-and-place demo.
  - Launcher, trigger commands, expected behavior, and observability workflow.

- [`guides/HMI_QUALIFICATION_CHECKLIST.md`](guides/HMI_QUALIFICATION_CHECKLIST.md)
  - Manual browser/HMI qualification checklist for release candidates and showcase runs.
  - Defines what remains manual after automated scene diagnostics and snapshot checks.

- [`reference/MCP_SCHEMA.md`](reference/MCP_SCHEMA.md)
  - Contract reference for the ManyForge MCP server.
  - Tool-by-tool interface schema, parameters, and semantics.

- [`reference/REFERENCE.md`](reference/REFERENCE.md)
  - Engineering reference for contributors and sandboxed coding agents.
  - Core C++ conventions and design patterns carried forward from ManyMove.

- [`plans/PRODUCT_DIRECTION_PLAN.md`](plans/PRODUCT_DIRECTION_PLAN.md)
  - Top-level roadmap for the next phase of ManyForge.
  - Product thesis, priority bands, public-preview target, and execution order.

- [`plans/INDUSTRIAL_EXECUTION_READINESS_PLAN.md`](plans/INDUSTRIAL_EXECUTION_READINESS_PLAN.md)
  - Main execution/readiness plan for industrial credibility.
  - External integration, interrupt proof, verification layers, GUI qualification,
    and evidence-backed readiness criteria.

- [`plans/AI_ASSISTANT_INTEGRATION_PLAN.md`](plans/AI_ASSISTANT_INTEGRATION_PLAN.md)
  - Live plan for bounded model use, Composer assistant work, and OpenClaw/NemoClaw integration.
  - Defines what stays in ManyForge, what belongs to supervisory layers, and what is optional or deferred.

- [`plans/PREVIEW_AND_ADOPTION_PLAN.md`](plans/PREVIEW_AND_ADOPTION_PLAN.md)
  - Short-term milestone plan for the first useful, presentable ManyForge story.
  - Coordinates simulation-backed adoption and assistant-backed authoring without conflicting with the longer-term roadmap.

- [`plans/ISAAC_SIM_PREVIEW_PLAN.md`](plans/ISAAC_SIM_PREVIEW_PLAN.md)
  - Tactical simulator-backed sub-plan for the first preview implementation.
  - Keeps the simulator control path open between direct, ROS-wrapped, and hybrid approaches.

- [`plans/INTERRUPT_MODEL_PLAN.md`](plans/INTERRUPT_MODEL_PLAN.md)
  - Technical reference for the interrupt execution model.
  - Interrupt sources, required semantics, current baseline, and remaining proof work.

- [`../verification/scenarios/basic_pick_and_place_complete.scenario.yaml`](../verification/scenarios/basic_pick_and_place_complete.scenario.yaml)
  - Baseline end-to-end pick-and-place qualification scenario.
  - Smallest initial scenario artifact for the qualification harness.

- [`../verification/scenarios/interrupt_pick_and_place_pause_resume.scenario.yaml`](../verification/scenarios/interrupt_pick_and_place_pause_resume.scenario.yaml)
  - Interrupt hold/resume qualification scenario for the interrupt pick-and-place flow.
  - Verifies handler execution, resumable hold, and clean completion.

- [`../verification/matrices/core_qualification.yaml`](../verification/matrices/core_qualification.yaml)
  - Initial deterministic qualification batch.
  - Groups the core end-to-end scenarios used by the first qualification tier.

- [`../verification/matrices/native_live_evaluation.yaml`](../verification/matrices/native_live_evaluation.yaml)
  - Native planner/executor qualification batch.
  - Exercises a UR5 joint-goal program against the real RoboPlan/native kernel path.

- [`../verification/matrices/soak_qualification.yaml`](../verification/matrices/soak_qualification.yaml)
  - Repeated deterministic qualification batch.
  - Used for liveness and deadlock detection across repeated runs.

- [`operations/STACK_SETUP.md`](operations/STACK_SETUP.md)
  - Compact operational runbook for local bring-up and current stack modes.
  - Commands, scripts, and troubleshooting notes.

- [`integration/PLATFORM_REQUIREMENTS.md`](integration/PLATFORM_REQUIREMENTS.md)
  - Upstream platform gaps and required changes in OpenShell, OpenClaw, and
    NemoClaw-Thor.
  - Explains why the current mux/plugin workaround still exists.

## Template / Artifact Docs

These are not the main source-of-truth docs. They are payloads used by scripts
or copied into sandbox/plugin environments.

- [`../scripts/templates/AGENTS.md`](../scripts/templates/AGENTS.md)
  - Template agent reference uploaded into sandbox/workspace contexts.

- [`../scripts/templates/SOUL.md`](../scripts/templates/SOUL.md)
  - Template safety/behavior boundary document for sandbox agent contexts.

- [`../scripts/templates/skills/manyforge-monitor-execution/SKILL.md`](../scripts/templates/skills/manyforge-monitor-execution/SKILL.md)
  - Skill template for runtime monitoring.

- [`../scripts/templates/skills/manyforge-diagnose-failure/SKILL.md`](../scripts/templates/skills/manyforge-diagnose-failure/SKILL.md)
  - Skill template for failure diagnosis.

- [`../scripts/templates/skills/manyforge-intervene-recovery/SKILL.md`](../scripts/templates/skills/manyforge-intervene-recovery/SKILL.md)
  - Skill template for intervention-based recovery.

- [`../scripts/templates/skills/manyforge-escalate-to-human/SKILL.md`](../scripts/templates/skills/manyforge-escalate-to-human/SKILL.md)
  - Skill template for operator escalation.

- [`../scripts/templates/skills/manyforge-delegated-recovery/SKILL.md`](../scripts/templates/skills/manyforge-delegated-recovery/SKILL.md)
  - Skill template for delegated recovery workflows.

## How To Use This Set

- Start with `README.md` if you are new to the repo.
- Start with this file if you already know the repo and need the internal doc
  map.
- Treat the files under `docs/` as the authoritative docs.
- Treat template docs under `scripts/templates/` as runtime/sandbox artifacts,
  not planning documents.
