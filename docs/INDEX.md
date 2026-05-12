# ManyForge Runtime Documentation Index

`README.md` remains the public entrypoint. This index lists runtime/operator
documentation that travels with the implementation repository.

Development instructions, architecture specs, ADRs, plans, open points,
engineering references, LLM-agent development guidance, and contract documents
live in the sibling private specs repository:

- `../../manyforge_specs/AGENTS.md`
- `../../manyforge_specs/docs/INDEX.md`
- `../../manyforge_specs/docs/agent-playbook.md`
- `../../manyforge_specs/docs/implementation/manyforge-implementation-workflow.md`
- `../../manyforge_specs/docs/cross-workspace-conventions.md`
- `../../manyforge_specs/docs/spec/`
- `../../manyforge_specs/docs/adr/`
- `../../manyforge_specs/docs/plans/`
- `../../manyforge_specs/docs/reference/`

If `../../manyforge_specs` is unavailable, this code repository is still
runnable, but it is intentionally missing the authoritative development
guidance required for LLM-assisted development.

## Runtime And Operator Docs

- [`../README.md`](../README.md)
  - User-facing project entrypoint.

- [`operations/STACK_SETUP.md`](operations/STACK_SETUP.md)
  - Compact operational runbook for local bring-up and current stack modes.

- [`operations/ASSISTANT_E2E_COOKBOOK.md`](operations/ASSISTANT_E2E_COOKBOOK.md)
  - Practical E2E assistant recipes, known-good demo launch, and model/profile
    smoke checks.

- [`guides/INTERRUPT_DEMO.md`](guides/INTERRUPT_DEMO.md)
  - Operator guide for the interrupt-focused pick-and-place demo.

- [`reference/ROBOPLAN_ADAPTER_NOTES.md`](reference/ROBOPLAN_ADAPTER_NOTES.md)
  - Runtime notes for RoboPlan adapter projection rules and temporary backend
    workarounds.

## Runtime Template Artifacts

These files are copied into OpenClaw/NemoClaw runtime sandboxes. They are
runtime payloads, not repo-level development instructions.

- [`../scripts/templates/AGENTS.md`](../scripts/templates/AGENTS.md)
- [`../scripts/templates/SOUL.md`](../scripts/templates/SOUL.md)
- [`../scripts/templates/skills/manyforge-monitor-execution/SKILL.md`](../scripts/templates/skills/manyforge-monitor-execution/SKILL.md)
- [`../scripts/templates/skills/manyforge-diagnose-failure/SKILL.md`](../scripts/templates/skills/manyforge-diagnose-failure/SKILL.md)
- [`../scripts/templates/skills/manyforge-intervene-recovery/SKILL.md`](../scripts/templates/skills/manyforge-intervene-recovery/SKILL.md)
- [`../scripts/templates/skills/manyforge-escalate-to-human/SKILL.md`](../scripts/templates/skills/manyforge-escalate-to-human/SKILL.md)
- [`../scripts/templates/skills/manyforge-delegated-recovery/SKILL.md`](../scripts/templates/skills/manyforge-delegated-recovery/SKILL.md)
