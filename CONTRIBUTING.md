# Contributing to ManyForge

This file is the entry point for both **human contributors** and
**LLM-driven agents**. Both audiences are first-class — the workflow
below works for either.

If you're an LLM, read this file end-to-end before making any change.
You should also read [AGENTS.md](AGENTS.md) which is the durable
machine-readable companion to this doc.

## Short version

1. **`dev` is the integration branch; `main` is for released code only.**
2. **Only the maintainer pushes to upstream `dev`.** External
   contributors fork the repo and open a pull request from their fork
   to upstream `dev`.
3. **Releases**: the maintainer squash-merges `dev → main` when a new
   version lands, then tags. (Mirrors the
   [manymove](https://github.com/pastoriomarco/manymove) policy.)
4. **Normal development is spec-driven.** Maintainer work should use
   the sibling private `manyforge_specs` repo as the canonical source
   for contracts, ADRs, and development plans.
5. CI must stay green on `dev`.

## Repository structure (for orientation)

- `manyforge_core/` — C++ kernel, state machine, ports.
- `manyforge_roboplan/` — C++ planner / context / execution backends
  (native + ros2_control).
- `manyforge_behavior/` — Python py_trees behavior-tree runner.
- `manyforge_composer/` — FastAPI backend + React frontend for the
  authoring UI.
- `manyforge_hmi/` — Viser-based 3D HMI.
- `manyforge_assistant_bridge/` — Assistant-provider HTTP bridge +
  status supervisor.
- `manyforge_ros/`, `manyforge_ros_bringup/` — ROS adapter modules
  and bringup helpers.
- `scripts/` — operator-facing launchers (`launch.sh` is the
  unified entry; `demo-*.sh` are backward-compat shims).
- `docs/` — operator-facing guides, runbooks, reference.

Specs and ADRs are in a separate private repository (`manyforge_specs`)
and are not required at runtime. They **are** required for maintainer
development that changes behavior, contracts, state machines, schemas,
execution semantics, assistant policy, or architecture. If you do not
have access to `manyforge_specs`, you can still contribute bounded
implementation fixes, tests, docs, and issue reports, but non-trivial
behavior changes need maintainer audit against the specs before merge.

Public issues and PRs should describe **behavior**, not private spec
paths. The CI guard `scripts/ci/check-private-spec-boundary.sh`
enforces this boundary.

## For humans

### Setup

The build runs entirely inside Docker — there is no host install path
for dev. Prerequisites: Docker Engine + Docker Compose v2, plus `git`.

```bash
git clone https://github.com/pastoriomarco/manyforge.git
cd manyforge
./scripts/setup-docker.sh        # build the dev image
./scripts/launch.sh              # interactive wizard
```

See [README.md](README.md) for quickstart variants and
[docs/operations/STACK_SETUP.md](docs/operations/STACK_SETUP.md) for the
operational runbook.

### Local checks (both flows)

Run the relevant check before committing. These run the same way
whether you're the maintainer or a forked contributor:

- C++ kernel: `./scripts/test-all.sh cpp`
- C++ RoboPlan: `./scripts/test-all.sh roboplan`
- Python: `./scripts/test-all.sh python`
- Frontend: `cd manyforge_composer/frontend && npm run build`
- ROS adapter contracts: `docker compose --profile ros run --rm ros-jazzy ./scripts/test-ros.sh`
- Full ros2_control contracts: `docker compose --profile ros-full run --rm ros-jazzy-full ./scripts/test-ros-full.sh`
- All bash: `bash -n scripts/*.sh scripts/lib/*.sh`
- Version sync: `./scripts/ci/verify-version-sync.sh`

Commit with a Conventional-Commits-ish prefix: `feat:`, `fix:`,
`docs:`, `refactor:`, `test:`, `chore:`.

### Maintainer flow (direct push to upstream `dev`)

Only the maintainer has push access to upstream `dev`. The flow is:

1. `git checkout dev && git pull` — start from integration head.
2. Make changes; run the local checks above.
3. `git commit` and `git push origin dev`.
4. CI runs automatically on `dev`: Python tests, frontend build, bash
   syntax, version sync, private-spec boundary, cpp-tsan (when
   `manyforge_core` is touched). Keep `dev` green.

### External contributor flow (fork + PR)

1. **Fork** the repository on GitHub.
2. Clone your fork; add upstream as a remote:
   ```bash
   git clone https://github.com/<you>/manyforge.git
   cd manyforge
   git remote add upstream https://github.com/pastoriomarco/manyforge.git
   ```
3. Sync your fork's `dev` with upstream before starting:
   ```bash
   git fetch upstream
   git checkout dev && git reset --hard upstream/dev
   ```
4. Either work on `dev` in your fork, or create a topic branch:
   `git checkout -b feature/<short-desc>` (also `fix/<x>` or
   `docs/<x>`).
5. Make changes; run the local checks above; commit.
6. Push to your fork: `git push origin <branch>` (or `dev`).
7. Open a pull request from your fork → upstream `dev`.
8. CI runs on the PR. Address review comments by pushing more
   commits to the same branch on your fork.

### Cutting a release

Releases happen when `dev` has accumulated enough work to warrant a
version bump. Maintainer flow:

1. For the first public release, finalize the existing `0.1.0 - Upcoming`
   section in `CHANGELOG.md`. For later releases, bump `VERSION` (SemVer) and
   add a new changelog section on `dev`.
2. Confirm CI is green on `dev`.
3. **Squash-merge `dev → main`** — one commit per release, as
   manymove does. The squash message is the release notes.
4. Tag the merge commit, for example `git tag v0.1.0 && git push --tags`
   for the first public tag.
5. (Optional) Draft a GitHub release from the tag.

If you spot something that should go in the next release, mention it
in your commit / PR description so the maintainer can pull it into the
changelog.

### Coding conventions

- **C++**: ports-and-adapters architecture; no ROS types in
  `manyforge_core`; bindings stay thin. `set -euo pipefail` style for
  bash. Comments explain *why*, not *what*.
- **Python**: type hints encouraged on public APIs. `from __future__
  import annotations` at the top of any file using PEP-604 union
  syntax.
- **Frontend**: TypeScript strict mode, prefer Zustand selectors over
  prop drilling, avoid coupling to specific deployment YAMLs.
- **Tests**: every behavior change ships a test that would have failed
  before the change. C++ tests live in `<pkg>/test/`, Python tests in
  `<pkg>/tests/`.

## For LLM agents

These rules are operational, non-negotiable, and load-bearing. They
exist because the project mixes human-driven and LLM-driven work and
the guardrails need to be the same for both.

### Branch + commit rules

1. **`dev` is where work lands; `main` is off-limits.** Only the
   maintainer pushes to upstream `dev`. If you are running in the
   maintainer's checkout, an authorized push goes to `origin/dev` (or
   to a topic branch if the operator explicitly asks). If you are
   running in a contributor's fork, an authorized push goes to the
   fork's `dev` or topic branch — never to upstream. Never push to
   `main` from anywhere; only the maintainer does that, via the
   release squash-merge.
2. **`git commit` requires explicit human authorization.** This is a
   standing rule, regardless of how clearly the change is requested.
   Stage the diff, present it to the operator, wait for the literal
   word `commit`. Multi-step instructions like "fix everything" do
   not transitively authorize commits. (Reference: priority memory
   rule `feedback_commits_explicit_only.md`.)
3. **`git push` is a separate authorization** from `git commit`.
   Wait for an explicit `push` reply.
4. **Never open a pull request automatically.** Use the GitHub UI or
   wait for the operator. Do not run `gh pr create` unless explicitly
   instructed.
5. **Releases (squash-merge `dev → main` + tag) are operator-driven.**
   Do not bump `VERSION` or write `CHANGELOG.md` release sections
   without an explicit ask.

### Before any non-trivial change

Read the relevant existing code first. The project has a strong
"behavior, not assumption" rule: if you're touching the kernel state
machine, the assistant provider, or any port interface, find the
existing tests and the existing call sites before proposing the
change.

Then check whether the change is spec-governed. If it changes a public
contract, adapter boundary, deployment schema, state transition,
execution safety/stop behavior, assistant authority, or observability
surface, the expected maintainer workflow is:

1. Read the relevant material in the sibling `manyforge_specs` repo.
2. Confirm the implementation change matches the spec/ADR/plan.
3. If the spec is missing, ambiguous, or wrong, stop and ask for
   maintainer direction before changing code.

If `manyforge_specs` is unavailable, state that limitation in your
analysis or PR. That does not block small fixes, but it does mean the
change needs maintainer audit before it can be treated as a
spec-compliant behavior change.

Before opening a PR with a substantial diff, run
[`/codex:review`](https://github.com/openai/codex) (or equivalent
second-pass review) and iterate until it returns clean or the only
remaining suggestions are ones you've deliberately declined. Reference:
memory rule `feedback_codex_review_convergence.md`.

### When you encounter a constraint you can't resolve

Stop and ask the operator. Specifically:
- If specs in `manyforge_specs/` (private) conflict with the change
  you're being asked to make, surface the conflict — do not
  unilaterally rewrite spec text. (Reference:
  `feedback_respect_manyforge_specs.md`.)
- If `manyforge_specs/` is unavailable and the request is
  spec-governed, keep the change to a narrow implementation proposal or
  explicitly mark it as requiring maintainer audit.
- If you find dead code, ambiguous file paths, or a workflow doc that
  contradicts what the code does — flag it before proceeding.

### Self-check before submitting

- `./scripts/ci/verify-version-sync.sh` passes.
- `bash -n` clean on any bash files touched.
- `npx tsc --noEmit -p tsconfig.app.json` clean for frontend changes.
- Relevant pytest / ctest target passes for code changes.
- CHANGELOG.md updated under the upcoming release section for user-visible
  changes.
- AGENTS.md / CONTRIBUTING.md / README updated only when the durable
  workflow changes — never for transient incident notes.
- **Launcher behavior changed?** If your change touches
  `scripts/launch.sh`, `scripts/lib/*.sh`, `scripts/lib/scenarios.sh`,
  or any `scripts/demo-*.sh` shim, update
  [`docs/operations/LAUNCHER_QUICKSTART.md`](docs/operations/LAUNCHER_QUICKSTART.md)
  in the same commit and (when relevant)
  [`scripts/MIGRATION.md`](scripts/MIGRATION.md) too. The quickstart
  is the operator-facing smoke recipe; out-of-date quickstart =
  bricked first-run for new operators and LLM agents.

## Reporting issues

- **Bugs**: open a GitHub issue using the bug-report template.
  Include the lane (`manyforge-only` / `isaac-sim`), the scenario or
  deployment YAML you ran, and the relevant logs.
- **Security**: do NOT open a public issue. Email
  [pastoriomarco@gmail.com](mailto:pastoriomarco@gmail.com); see
  [SECURITY.md](SECURITY.md) for the disclosure process.
- **Features / discussions**: open an issue using the feature-request
  template, or start a GitHub Discussion if the proposal is broad
  enough to need scoping before a PR.

## Cross-repo coordination

ManyForge depends on [NemoClaw-Thor](https://github.com/pastoriomarco/NemoClaw-Thor)
for the assistant lane (vLLM serving + OpenClaw bridge). If your
change touches the assistant-provider HTTP contract or the deployment
YAML schema for the assistant block, coordinate with Thor by:

- Reading Thor's [CONTRIBUTING](https://github.com/pastoriomarco/NemoClaw-Thor/blob/main/CONTRIBUTING.md)
  to understand the wire-contract conventions on the other side.
- Opening linked PRs in both repos when a change crosses the boundary.

## License

By contributing to ManyForge you agree that your contributions will be
licensed under the [Apache License 2.0](LICENSE).
