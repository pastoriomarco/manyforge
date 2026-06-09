# ManyForge — agent landing

> **For LLMs (Claude / ChatGPT) helping the user with this repo:** read
> this file first. It tells you what `manyforge` is, what it ships, how
> to operate it at runtime (with or without the LLM assistant), and
> where to find the canonical reference docs. Read referenced docs
> before editing code or running scripts.

---

## What this repo is

ManyForge is the deployment-side robot-execution framework: a behavior
tree runtime, motion planner, scene/program/blackboard data model, a
Composer (web UI + FastAPI backend + native 3D viewer), a legacy Viser
HMI fallback, and an LLM assistant-provider HTTP bridge. **Everything
required to install,
deploy, and operate ManyForge at runtime is in this repo**, including
when the optional Composer-assistant lane is enabled.

Three sibling repositories form the full stack; the cross-repo
authority map is:

| Question | Authoritative repo | Notes |
|---|---|---|
| **How do I install / deploy / run ManyForge?** | this repo (`manyforge`) | self-sufficient at runtime — no other repo required for non-AI use |
| **How do I run the LLM assistant on Jetson Thor?** | `NemoClaw-Thor` | platform glue: vLLM serving, sandbox provisioner, OpenClaw bridge launcher. Required only when using the `openclaw` lane on Thor. |
| **What is the spec / contract / ADR — what should this code do?** | `manyforge_specs` (private) | dev-only; **not required at runtime**. Specs/ADRs/plans live there for maintainer use. |

### Repository locations and fallbacks

Prefer local checkouts under `${HOME}/workspaces/dev_ws/src/`. If a referenced
repo is missing locally, inspect or clone the matching repository under
`https://github.com/pastoriomarco/` before treating a local link as stale.

| Repo | Preferred local path | Accepted alternate local path | Remote |
|---|---|---|---|
| `manyforge` | `${HOME}/workspaces/dev_ws/src/manyforge` | - | `https://github.com/pastoriomarco/manyforge` |
| `NemoClaw-Thor` | `${HOME}/workspaces/dev_ws/src/NemoClaw-Thor` | `${HOME}/workspaces/nemoclaw/src/NemoClaw-Thor` | `https://github.com/pastoriomarco/NemoClaw-Thor` |
| `manyforge_specs` | `${HOME}/workspaces/dev_ws/src/manyforge_specs` | - | `https://github.com/pastoriomarco/manyforge_specs` |
| `isaac_ros_custom_bringup` | `${HOME}/workspaces/dev_ws/src/isaac_ros_custom_bringup` | `${HOME}/workspaces/isaac_ros-dev/src/isaac_ros_custom_bringup` | `https://github.com/pastoriomarco/isaac_ros_custom_bringup` |

**This file covers the runtime/operator side completely.** A brief
section at the bottom points dev maintainers at `manyforge_specs` for
spec-level work; that's the only reason an operator/LLM should ever
follow that link.

For development, the distinction is load-bearing: ManyForge is
implemented according to `manyforge_specs`. If that repo is missing,
runtime operation is still fully supported, but non-trivial behavior
development is limited and needs maintainer audit before it should be
treated as spec-compliant.

---

## Install

Container-based, single command:

```bash
git clone <manyforge>            # this repo
cd manyforge
./scripts/setup-docker.sh        # clones RoboPlan dependency + builds manyforge-dev:latest (~10 min first time)
docker compose run --rm dev bash # enter the dev container
./scripts/test-all.sh            # builds C++ core + runs all tests
```

Hard requirements: Docker (with BuildKit), git, ~10 GB disk for the
Docker image, and the host Python toolchain only if you want to run
scripts outside the container.

For a comprehensive operator runbook (deterministic-only mode,
direct-LLM mode, full assistant mode), see
[`docs/operations/STACK_SETUP.md`](docs/operations/STACK_SETUP.md).
For a freshly formatted machine that needs the whole ManyForge +
NemoClaw/OpenClaw assistant stack, start with
[`docs/operations/STACK_SETUP.md`](docs/operations/STACK_SETUP.md#clean-machine-install-order)
(`scripts/launch.sh` is a runtime launcher, not the first-time NemoClaw
installer). On Jetson Orin AGX, use
[`docs/operations/ORIN_STACK_SETUP.md`](docs/operations/ORIN_STACK_SETUP.md)
instead so the NVMe/eMMC storage contract is handled first.

---

## Runtime

ManyForge has **three deployment modes** — pick the one that matches
your needs. Each mode adds optional surface; the simpler ones are
self-contained.

**Operator-facing launcher quickstart:**
[`docs/operations/LAUNCHER_QUICKSTART.md`](docs/operations/LAUNCHER_QUICKSTART.md)
is the canonical guide for the unified launcher
(`scripts/launch.sh`). The primary use is the **interactive wizard**
(7-step NemoClaw-onboard-style menu — just run `./scripts/launch.sh`
with no arguments). The doc also covers scripted / non-interactive
use for CI, the four-combination smoke-test matrix, and the seven
`demo-*.sh` backward-compat shims. Read it before bringing up the
stack for the first time, when verifying a launcher change, or when
an end-to-end issue needs a known-good reference. **If you (LLM
agent) modify the launcher scripts, update that doc in the same
commit.**

**Adding a new robot deployment:** see
[`docs/operations/ADDING_A_ROBOT.md`](docs/operations/ADDING_A_ROBOT.md)
for URDF/SRDF requirements, deployment-YAML schema (arm + gripper),
capability adapter selection, and smoke-test curls.

### Mode 1 — Composer + behavior runtime (no LLM)

The full ManyForge runtime, no AI. Use for validating behavior trees,
debugging planning/execution, or any deployment that doesn't need an
assistant.

```bash
docker compose run --rm dev bash
python -m manyforge_composer \
  --catalog-path manyforge_behavior/resources/node_catalog.yaml \
  --host 0.0.0.0 --port 9000
```

Open `http://localhost:9000/` for the Composer UI and native viewer.
The legacy Viser HMI is available on `http://localhost:8081/` only if
Composer is launched with `--hmi-port 8081`.

### Mode 2 — Composer + direct LLM endpoint

Adds the **direct-lane assistant**: ManyForge's
`manyforge_assistant_bridge` (in this repo) runs a bounded multi-turn
loop against an OpenAI-compatible local LLM (typically vLLM). No
sandbox, no OpenClaw — fast iteration, sandbox-bypass.

```bash
# 1. Start an OpenAI-compatible server on :8000 (vLLM, llama.cpp, ...).
# 2. Start the bridge:
cd manyforge_assistant_bridge
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python -m manyforge_assistant_bridge.bridge   # listens on :8100
# 3. Start Composer with --assistant-provider nemoclaw:
docker compose run --rm dev bash -c '
  python -m manyforge_composer \
    --catalog-path manyforge_behavior/resources/node_catalog.yaml \
    --assistant-provider nemoclaw \
    --assistant-endpoint http://127.0.0.1:8100/v1/manyforge/assistant \
    --host 0.0.0.0 --port 9000'
```

Wire contract for the bridge service:
[`docs/reference/ASSISTANT_PROVIDER_CONTRACT.md`](docs/reference/ASSISTANT_PROVIDER_CONTRACT.md).

### Mode 3 — Composer + sandboxed assistant lane (OpenClaw)

Production default on Thor. Routes the assistant through a NemoClaw
sandbox running OpenClaw, which dispatches tool calls back to Composer
through an MCP bridge. Adds bounded-autonomy enforcement, per-tool
audit, and sandbox isolation. **Requires the `NemoClaw-Thor` repo**
for the Thor platform glue (vLLM container, sandbox provisioner,
OpenClaw launcher).

One-command bring-up via the demo launcher:

```bash
./scripts/demo-assistant-known-good.sh start
```

The launcher chains: vLLM start → NemoClaw provider config → Composer
launch → deployment load → OpenClaw provisioner → bridge launch →
status. See the script header for env-var overrides
(`MODEL_PROFILE`, `NEMOCLAW_THOR`, `ASSISTANT_PROVIDER`, etc.).

Production model default (2026-05-08): `cosmos-reason2-8b`. Operator
runbook for this lane lives in NemoClaw-Thor at
`manyforge/docs/COMPOSER-ASSISTANT-RUNBOOK.md`; lane-comparison
benchmark at `manyforge/docs/LANE-COMPARISON.md`.

---

## Operator-facing reference docs (this repo)

These three are the canonical runtime contracts. They live in this
repo so an operator never needs to fetch private specs to operate
ManyForge:

| Topic | Doc | Source-of-truth |
|---|---|---|
| Composer REST API | [`docs/reference/REST_API.md`](docs/reference/REST_API.md) | FastAPI app's `/openapi.json` (live at `:9000/openapi.json` when Composer is running) |
| MCP runtime contract | [`docs/reference/MCP_RUNTIME_CONTRACT.md`](docs/reference/MCP_RUNTIME_CONTRACT.md) | `manyforge_composer/backend/mcp_server.py` (`@mcp.tool()` decorators) |
| Assistant-provider HTTP | [`docs/reference/ASSISTANT_PROVIDER_CONTRACT.md`](docs/reference/ASSISTANT_PROVIDER_CONTRACT.md) | `manyforge_assistant_bridge/bridge.py` + `protocol.py` |

These docs are auto-generated from code (FastAPI's `openapi()` output
plus the FastMCP tool registry). To regenerate:

```bash
docker compose run --rm dev python3 scripts/regen-public-contracts.py
```

The script must run inside the manyforge dev image — it imports
`manyforge_composer.backend.app` / `mcp_server` which require the
container's installed deps (FastAPI, FastMCP, kernel client). Running
on the host without those deps produces a failure stub.

A CI guard verifies the docs stay in sync with the code:

```bash
bash scripts/ci/check-public-contracts-fresh.sh
```

It runs the regenerator in `--check` mode (exit 1 on drift). Wire this
into pre-commit / GitHub Actions to catch silent staleness — exactly
the failure mode that left these docs frozen against `build_app` /
`build_mcp_server` after those factories were renamed to `create_app`
/ `create_mcp_server`, until the next manual regen.

For workflow-level recipes (cold boot, profile bake-offs, lane
comparison, smoke tests), see
[`docs/operations/ASSISTANT_E2E_COOKBOOK.md`](docs/operations/ASSISTANT_E2E_COOKBOOK.md).

For all docs in this repo:
[`docs/INDEX.md`](docs/INDEX.md).

---

## Subcomponent map

What lives where in this repo, and what it's for:

| Path | What |
|---|---|
| [`manyforge_core/`](manyforge_core/) | C++20 core: planning DTOs, kernel state machine, ports |
| [`manyforge_behavior/`](manyforge_behavior/) | Behavior-tree runtime (py_trees), node catalog, tests |
| [`manyforge_roboplan/`](manyforge_roboplan/) | Native motion-planner backend bindings (RRT + TOPPRA) |
| [`manyforge_composer/`](manyforge_composer/) | FastAPI backend + React frontend + MCP server |
| [`manyforge_assistant_bridge/`](manyforge_assistant_bridge/) | Direct-lane assistant provider HTTP service (port 8100) |
| [`manyforge_hmi/`](manyforge_hmi/) | Legacy Viser-based 3D viewer fallback |
| [`manyforge_ros/`](manyforge_ros/) | ROS 2 bridge (optional) |
| [`agent-skills/manyforge-composer/`](agent-skills/manyforge-composer/) | OpenClaw skill manifest (used by Mode 3) |
| [`examples/`](examples/) | Reference `*.program.yaml` + `*.deployment.yaml` |
| [`scripts/`](scripts/) | Operator scripts (`demo-assistant-known-good.sh`, `setup-docker.sh`, etc.) |
| [`docs/`](docs/) | Operator docs (`operations/`, `guides/`, `reference/`) |

---

## Boundary rules for LLM agents working in this repo

When making changes here, check the boundary you're touching:

0. **Spec-governed behavior requires `manyforge_specs`.** Kernel state
   transitions, execution semantics, adapter contracts, deployment
   schemas, assistant authority, observability surfaces, and ADR-level
   architecture must be checked against the sibling private specs repo
   before implementation. Without that repo, keep changes narrow,
   document the limitation, and flag the work for maintainer audit.
1. **Don't reintroduce private-spec links** in any runtime path. The
   CI guard at [`scripts/ci/check-private-spec-boundary.sh`](scripts/ci/check-private-spec-boundary.sh)
   enforces this; run it before committing if your change touches docs
   under `docs/operations/`, `docs/reference/`, `docs/guides/`,
   `manyforge_assistant_bridge/`, `manyforge_composer/`, etc. The
   guard has an explicit allowlist for docs that legitimately *describe*
   the boundary (this file, `STACK_SETUP.md`, the dev-only session
   script).
2. **Operator-facing reference docs are derived from code, not specs.**
   See `scripts/regen-public-contracts.py`. Don't hand-copy spec
   content into `docs/reference/`; extend the generator instead.
3. **Sibling-repo paths use env-var defaults**: `MANYFORGE_ROOT`,
   `NEMOCLAW_THOR`, `MANYFORGE_SPECS_ROOT`. Never hard-code another
   user's home directory.
4. **The C++ core builds inside the dev container.** Don't try to
   build it on the host; the entrypoint handles paths and pybind11.
5. **Tests fall into deterministic + assistant lanes.** Run
   `./scripts/test-all.sh` for the deterministic suite. The assistant
   bridge has its own pytest suite under
   `tests/test_assistant_bridge*.py`.
6. **Schema changes are wire-breaking.** If you touch
   `manyforge_assistant_bridge/protocol.py` (the catalog-hash function
   or any shared envelope helper), every running deployment will
   recompute hashes — treat as a wire-version bump.

---

## Branch & commit workflow for agents

This repo uses a two-branch model (mirroring
[manymove](https://github.com/pastoriomarco/manymove)) that applies to
humans and LLM agents equally:

- **`main`** — released. Tagged. Protected. Off-limits to direct push.
- **`dev`** — integration. Where work lands. **Only the maintainer
  pushes to upstream `dev`.** External contributors fork the repo and
  open a PR from their fork to upstream `dev`.
- **Topic branches** off `dev` — optional. Used when a change is
  large enough to warrant pre-merge review, or when an external
  contributor opens a PR.

**Rules for LLM agents** (load-bearing, do not skip):

1. **Never push to `main`.** Only the maintainer does that, via the
   release squash-merge from `dev`. If you are running in the
   maintainer's checkout, an authorized push goes to `origin/dev` (or
   a topic branch if the operator explicitly asks). If you are
   running in a contributor's fork, an authorized push goes to the
   fork — never directly to upstream `dev` or `main`.
2. **`git commit` requires explicit human authorization** — every time.
   Stage the diff, present it, wait for the literal word `commit`. The
   priority memory rule `feedback_commits_explicit_only.md` is operative:
   multi-step instructions like "fix everything" do not transitively
   authorize commits.
3. **`git push` is a separate authorization** from `git commit`. Wait
   for an explicit `push` reply.
4. **Never open a PR autonomously.** Do not run `gh pr create` unless
   explicitly asked. The operator opens the PR through the GitHub UI
   when they're ready.
5. **Before a non-trivial push, run `/codex:review`** (or equivalent
   second-pass review) and iterate to convergence. Reference:
   `feedback_codex_review_convergence.md`.
6. **Releases are operator-driven.** Do not bump `VERSION`, do not
   write `[X.Y.Z]` sections in `CHANGELOG.md`, do not tag, do not
   squash-merge `dev → main` — those are explicit-ask operations.

The per-package version-of-record is the repo-root `VERSION` file. The
CI workflow `version-sync.yml` (driven by
`scripts/ci/verify-version-sync.sh`) fails any PR or direct push to
`dev`/`main` whose sub-package versions drift from `VERSION`.

Full contributor reference, applicable to both humans and LLMs:
[`CONTRIBUTING.md`](CONTRIBUTING.md).

---

## For maintainers / spec work

If your task is to **change a contract, add an ADR, modify a spec, or
work on a development plan**, you need the private `manyforge_specs`
repo cloned alongside `manyforge` (sibling directory). The same applies
to non-trivial implementation work that changes externally observable
behavior or the architectural contract. It is **not** required for any
runtime / install / operator task; only for maintainer development
workflows.

Authoritative dev-side instructions:

- `../manyforge_specs/AGENTS.md`
- `../manyforge_specs/docs/agent-playbook.md`
- `../manyforge_specs/docs/cross-workspace-conventions.md`
- `../manyforge_specs/docs/spec/`
- `../manyforge_specs/docs/adr/`

If `../manyforge_specs/` isn't present, first check whether it exists at
`${HOME}/workspaces/dev_ws/src/manyforge_specs` or can be cloned from
`https://github.com/pastoriomarco/manyforge_specs`. If it is still unavailable,
it means this checkout is for *deployment* use or limited implementation work,
not full maintainer development. **Don't fail the operator's runtime task** —
answer their runtime question from this repo's content (which is complete for
operation). For code changes, be explicit: small fixes may proceed from local
code and tests, but spec-governed behavior changes need maintainer audit
against `manyforge_specs` before merge.

To seed an OpenClaw agent sandbox with the specs repo for development
assistance, the dev-only script is at
[`scripts/dev-agent-session-start.sh`](scripts/dev-agent-session-start.sh).
It refuses to run without `manyforge_specs` on disk by design.
