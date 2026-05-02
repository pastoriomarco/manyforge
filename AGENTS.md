# ManyForge Agent Entry Point

This repository contains the ManyForge implementation: code, tests, runtime
artifacts, examples, Docker lanes, and operator launch scripts.

Authoritative development instructions live in the sibling private specs
repository:

- `../manyforge_specs/AGENTS.md`
- `../manyforge_specs/docs/agent-playbook.md`
- `../manyforge_specs/docs/implementation/manyforge-implementation-workflow.md`
- `../manyforge_specs/docs/cross-workspace-conventions.md`
- `../manyforge_specs/docs/spec/`
- `../manyforge_specs/docs/adr/`

Before making any change, read the relevant files in
`../manyforge_specs`. If that repository is unavailable, stop and tell the user
that this checkout is missing the required development-instruction repository.

ManyForge implementation docs under `docs/` are runtime/operator references
only. They do not override `manyforge_specs`.

Runtime sandbox templates under `scripts/templates/` are shipped artifacts for
LLM-assisted ManyForge operation through OpenClaw/NemoClaw. They are not
development instructions for this repository and must not be treated as a
repo-level `AGENTS.md`.
