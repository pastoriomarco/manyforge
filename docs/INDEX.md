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

- [`reference/MCP_SCHEMA.md`](reference/MCP_SCHEMA.md)
  - Contract reference for the ManyForge MCP server.
  - Tool-by-tool interface schema, parameters, and semantics.

- [`reference/REFERENCE.md`](reference/REFERENCE.md)
  - Engineering reference for contributors and sandboxed coding agents.
  - Core C++ conventions and design patterns carried forward from ManyMove.

- [`plans/LLM_OPERABILITY_PLAN.md`](plans/LLM_OPERABILITY_PLAN.md)
  - Main ManyForge-side plan for LLM/runtime behavior.
  - Current baseline, settled decisions, open work, and exit criteria.

- [`plans/OPENCLAW_INTEGRATION_PLAN.md`](plans/OPENCLAW_INTEGRATION_PLAN.md)
  - Companion integration plan for NemoClaw/OpenClaw-specific work.
  - Routing boundaries, integration modes, and remaining integration work.

- [`plans/INTERRUPT_MODEL_PLAN.md`](plans/INTERRUPT_MODEL_PLAN.md)
  - Execution-model plan for industrial runtime interrupts.
  - Interrupt sources, handler chains, required spec changes, and runtime impact.

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
