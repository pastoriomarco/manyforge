# ManyForge — agent landing

> **For LLMs (Claude / ChatGPT) helping the user with this repo:** read
> this file first. It tells you what `manyforge` is, what it ships, how
> to operate it at runtime (with or without the LLM assistant), and
> where to find the canonical reference docs. Read referenced docs
> before editing code or running scripts.

______________________________________________________________________

## What this repo is

ManyForge is the deployment-side robot-execution framework: a behavior
tree runtime, motion planner, scene/program/blackboard data model, Composer
(web UI + FastAPI backend + native 3D viewer), and an LLM assistant-provider
HTTP bridge. **Everything required to install,
deploy, and operate ManyForge at runtime is in this repo**, including
when the optional Composer-assistant lane is enabled.

Three sibling repositories form the full stack; the cross-repo
authority map is:

| Question                                                          | Authoritative repo          | Notes                                                                                                                             |
| ----------------------------------------------------------------- | --------------------------- | --------------------------------------------------------------------------------------------------------------------------------- |
| **How do I install / deploy / run ManyForge?**                    | this repo (`manyforge`)     | self-sufficient at runtime — no other repo required for non-AI use                                                                |
| **How do I run the LLM assistant on Jetson Thor?**                | `NemoClaw-Thor`             | platform glue: vLLM serving, sandbox provisioner, OpenClaw bridge launcher. Required only when using the `openclaw` lane on Thor. |
| **What is the spec / contract / ADR — what should this code do?** | `manyforge_specs` (private) | dev-only; **not required at runtime**. Specs/ADRs/plans live there for maintainer use.                                            |

### Repository locations and fallbacks

Prefer local checkouts under `${HOME}/workspaces/dev_ws/src/`. If a referenced
repo is missing locally, inspect or clone the matching repository under
`https://github.com/pastoriomarco/` before treating a local link as stale.

| Repo                       | Preferred local path                                     | Accepted alternate local path                                   | Remote                                                      |
| -------------------------- | -------------------------------------------------------- | --------------------------------------------------------------- | ----------------------------------------------------------- |
| `manyforge`                | `${HOME}/workspaces/dev_ws/src/manyforge`                | -                                                               | `https://github.com/pastoriomarco/manyforge`                |
| `NemoClaw-Thor`            | `${HOME}/workspaces/dev_ws/src/NemoClaw-Thor`            | `${HOME}/workspaces/nemoclaw/src/NemoClaw-Thor`                 | `https://github.com/pastoriomarco/NemoClaw-Thor`            |
| `manyforge_specs`          | `${HOME}/workspaces/dev_ws/src/manyforge_specs`          | -                                                               | `https://github.com/pastoriomarco/manyforge_specs`          |
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

______________________________________________________________________

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

______________________________________________________________________

## Runtime

Runtime selection has two independent axes:

- the **robot execution lane** is ManyForge-only native, Isaac commanded
  simulation, or a physical ROS-backed robot; and
- the optional **assistant provider** is off, `direct` (`nemoclaw` alias),
  `openclaw`, or `hermes`.

Changing the assistant provider never changes motion authority or robot-state
precedence. See
[`docs/reference/ARCHITECTURE.md`](docs/reference/ARCHITECTURE.md#deployment-lanes)
for execution lanes and
[`docs/operations/LANE_BRINGUP.md`](docs/operations/LANE_BRINGUP.md) for the
three engine-backed assistant lanes.

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

### Assistant off — Composer + behavior runtime

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

### Assistant `direct` — local model endpoint

Adds the **direct-lane assistant**: ManyForge's
`manyforge_assistant_bridge` (in this repo) runs a bounded multi-turn
loop against an OpenAI-compatible local LLM (typically vLLM). The standard
in-tree launcher starts this bridge as a host peer; a platform deployment may
instead place the same bridge in a policy-configured sandbox.

```bash
# 1. Start an OpenAI-compatible server on :8000 (vLLM, llama.cpp, ...).
# 2. Start the bridge:
cd manyforge_assistant_bridge
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python -m manyforge_assistant_bridge.bridge   # listens on :8100
# 3. Start Composer with the canonical direct provider id:
docker compose run --rm dev bash -c '
  python -m manyforge_composer \
    --catalog-path manyforge_behavior/resources/node_catalog.yaml \
    --assistant-provider direct \
    --assistant-endpoint http://127.0.0.1:8100/v1/manyforge/assistant \
    --host 0.0.0.0 --port 9000'
```

Wire contract for the bridge service:
[`docs/reference/ASSISTANT_PROVIDER_CONTRACT.md`](docs/reference/ASSISTANT_PROVIDER_CONTRACT.md).

### Assistant `openclaw` — sandboxed OpenClaw lane

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

Production model default (2026-06-11): `gemma4-12b-it-gguf` on Thor
(`gemma4-12b-it-gguf-orin` on Jetson Orin AGX via
`scripts/launch-orin-gemma.sh`). The launcher default is
`ASSISTANT_PROVIDER=openclaw` plus `MODEL_PROFILE=gemma4-12b-it-gguf`;
explicit `MODEL_PROFILE=...` still wins for bake-offs or historical
Cosmos reruns. Operator runbook for this lane lives in NemoClaw-Thor at
`manyforge/docs/COMPOSER-ASSISTANT-RUNBOOK.md`; lane-comparison
benchmark at `manyforge/docs/LANE-COMPARISON.md`.

### Assistant `hermes` — sandboxed Hermes lane

Hermes is the third registered engine-backed provider. It uses the same
Composer envelope and authority checks through a NemoClaw-Thor adapter on
port `8300`, while Hermes owns its session/agent loop. It is opt-in and its
sandbox is provisioned separately from OpenClaw. Select `--provider hermes`
with the unified launcher and follow
[`docs/operations/LANE_BRINGUP.md`](docs/operations/LANE_BRINGUP.md) for
provisioning, live checks, and teardown.

______________________________________________________________________

## Operator-facing reference docs (this repo)

These generated contracts are the exact public runtime wire surfaces. They
live in this repo so an operator never needs private specs to operate or
integrate ManyForge. Use
[`docs/reference/ARCHITECTURE.md`](docs/reference/ARCHITECTURE.md) for the
hand-maintained ownership and end-to-end flow map.

| Topic                   | Doc                                                                                                  | Source-of-truth                                                                                                   |
| ----------------------- | ---------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| Composer REST API       | [`docs/reference/REST_API.md`](docs/reference/REST_API.md)                                           | FastAPI app's `/openapi.json` (live at `:9000/openapi.json` when Composer is running)                             |
| MCP runtime contract    | [`docs/reference/MCP_RUNTIME_CONTRACT.md`](docs/reference/MCP_RUNTIME_CONTRACT.md)                   | `manyforge_composer/backend/mcp_server.py` (`@mcp.tool()` decorators)                                             |
| Assistant-provider HTTP | [`docs/reference/ASSISTANT_PROVIDER_CONTRACT.md`](docs/reference/ASSISTANT_PROVIDER_CONTRACT.md)     | `manyforge_assistant_bridge/bridge.py` + `protocol.py`                                                            |
| Viewer data plane       | [`docs/reference/VIEWER_DATA_PLANE_V1.schema.json`](docs/reference/VIEWER_DATA_PLANE_V1.schema.json) | Strict backend Pydantic wire models; the same generator emits frontend TypeScript types and preservation fixtures |

The Markdown contracts, viewer schemas/fixtures, and frontend viewer types are
generated from their owning FastAPI, FastMCP, protocol, and Pydantic models.
Do not edit generated outputs by hand. To regenerate:

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

______________________________________________________________________

## Subcomponent map

What lives where in this repo, and what it's for:

| Path                                                                     | What                                                                                                                        |
| ------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------- |
| [`manyforge_core/`](manyforge_core/)                                     | C++20 core: planning DTOs, kernel state machine, ports                                                                      |
| [`manyforge_content_identity/`](manyforge_content_identity/)             | Shared ROS-free RFC 8785 serialization and domain-separated content-digest primitives                                      |
| [`manyforge_persistence/`](manyforge_persistence/)                       | Shared ROS-free mechanical durable-storage safety primitives; no semantic state ownership                                  |
| [`manyforge_scene/`](manyforge_scene/)                                   | ROS-free logical-scene semantic owner and durable state                                                                     |
| [`manyforge_deployment/`](manyforge_deployment/)                         | ROS-free composable-cell artifact decoding, identity, validation, and compiler substrate                                    |
| [`manyforge_component_manager/`](manyforge_component_manager/)           | ROS-free C++ Component Manager runtime-observation and durable-persistence foundation                                       |
| [`manyforge_behavior/`](manyforge_behavior/)                             | Behavior-tree runtime (py_trees), node catalog, tests                                                                       |
| [`manyforge_collision/`](manyforge_collision/)                           | Planner-neutral immutable collision-world publication and authoritative query engine                                        |
| [`manyforge_roboplan/`](manyforge_roboplan/)                             | Retained planning-model staging and offline RoboPlan import qualification; runtime adapters are intentionally unavailable   |
| [`manyforge_ros2_control_execution/`](manyforge_ros2_control_execution/) | C++ ROS controller execution, exact binding/admission, feedback/progress, bounded stop/reset, and isolated scene projection |
| [`manyforge_robot_lifecycle/`](manyforge_robot_lifecycle/)               | ROS-independent lifecycle supervision, authority sessions, and admission projection                                         |
| [`manyforge_robot_lifecycle_msgs/`](manyforge_robot_lifecycle_msgs/)     | Robot-neutral ROS IDL for same-host lifecycle admission transport                                                           |
| [`manyforge_xarm_adapter/`](manyforge_xarm_adapter/)                     | Experimental xArm model generation, endpoint binding, and read-only observation boundary                                    |
| [`manyforge_xarm_control/`](manyforge_xarm_control/)                     | Experimental xArm lifecycle coordinator and sole-backend ros2_control boundary                                              |
| [`manyforge_composer/`](manyforge_composer/)                             | FastAPI backend + React frontend + MCP server                                                                               |
| [`manyforge_assistant_bridge/`](manyforge_assistant_bridge/)             | Direct-lane assistant provider HTTP service (port 8100)                                                                     |
| [`manyforge_ros/`](manyforge_ros/)                                       | ROS 2 bridge (optional)                                                                                                     |
| [`agent-skills/manyforge-composer/`](agent-skills/manyforge-composer/)   | ManyForge assistant skill installed by sandboxed provider deployments                                                       |
| [`examples/`](examples/)                                                 | Reference `*.program.yaml` + `*.deployment.yaml`                                                                            |
| [`scripts/`](scripts/)                                                   | Operator scripts (`demo-assistant-known-good.sh`, `setup-docker.sh`, etc.)                                                  |
| [`docs/`](docs/)                                                         | Operator docs (`operations/`, `guides/`, `reference/`)                                                                      |

______________________________________________________________________

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
1. **Operator-facing reference docs are derived from code, not specs.**
   See `scripts/regen-public-contracts.py`. Don't hand-copy spec
   content into `docs/reference/`; extend the generator instead.
1. **Sibling-repo paths use env-var defaults**: `MANYFORGE_ROOT`,
   `NEMOCLAW_THOR`, `MANYFORGE_SPECS_ROOT`. Never hard-code another
   user's home directory.
1. **Builds and tests run in containers, not on the host.** Host
   `cmake`, `pytest`, `npm`, or ROS setup is inspection-only and does
   not count as qualification evidence. Use `docker compose run --rm dev`
   for ROS-free core/RoboPlan/pybind11/Python/Composer checks; use
   `docker compose --profile ros run --rm ros-jazzy ...` for minimal ROS
   adapter checks; use `docker compose --profile ros-full run --rm ros-jazzy-full ...` or the launcher `isaac-sim` lane for full
   ROS/Isaac/ros2_control checks.
1. **Launcher lane matters.** `manyforge-only` / native
   `manyforge-e2e-composer` containers are ROS-free. They are valid for
   deterministic native tests only. ROS packages require the
   `isaac-sim` lane or the `ros-full` compose profile.
1. **Tests fall into deterministic + assistant lanes.** Run
   `./scripts/test-all.sh` inside the `dev` container for the
   deterministic suite. The assistant bridge has its own pytest suite
   under `manyforge_assistant_bridge/tests/`.
1. **Schema changes are wire-breaking.** If you touch
   `manyforge_assistant_bridge/protocol.py` (the catalog-hash function
   or any shared envelope helper), every running deployment will
   recompute hashes — treat as a wire-version bump.

______________________________________________________________________

## Temporary composable-cell destructive implementation window

> **TEMPORARY — remove when the Composable Cell Runtime Implementation Plan
> closes.**

This exception applies only to work explicitly mapped to the active
`COMPOSABLE_CELL_RUNTIME_IMPLEMENTATION_PLAN.md` in the sibling private
`manyforge_specs` repository. It applies only to local, unpushed `dev` work
while that plan remains active; it is not a general ManyForge development
policy.

For those plan slices only:

1. A local commit may intentionally leave one or all product lanes unavailable
   and need not pass the complete CI or three-lane qualification matrix.
1. Pre-commit validation may be limited to the focused layer, contract,
   syntax, static-analysis, and regression checks selected by the owning
   slice. Expected product-level failures or absent surfaces must be recorded
   in the plan's demolition ledger and named in the commit handoff; they must
   not be hidden with permissive fallbacks or misleading skipped tests.
1. Superseded implementations and their exclusive tests, configuration, and
   documentation may be deleted before an end-to-end replacement exists.
   Do not add a compatibility shim, backup implementation, copied legacy tree,
   or old/new runtime switch.
1. Operator documentation must remain truthful. When a documented surface is
   intentionally unavailable, mark or remove that recipe instead of repairing
   the implementation solely to preserve the old workflow.

This exception does **not** relax container-based validation, branch rules,
commit or push authorization, normative ADR/spec approval, private-spec
boundaries, robot/safety constraints, focused tests for changed foundational
layers, or adversarial review. For this plan, one full independent adversarial
review covers the cumulative diff of a qualification checkpoint: a named plan
acceptance criterion, a new or changed public/persisted/safety/ownership
boundary, or a restored or newly testable end-to-end capability. Multiple
semantically related commits may belong to that checkpoint. Incomplete
internal slices, mechanical migrations, fixtures, and evidence corrections
need focused tests and normal review but do not trigger a separate full review
unless they materially change architecture or safety semantics. Full
applicable CI and the named product gates must pass before the affected work
is pushed or claimed restored.

The I12/M-final plan-closing change must delete this entire section and the
matching section in `CONTRIBUTING.md`. The plan cannot be marked complete while
either temporary exception remains.

______________________________________________________________________

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
1. **`git commit` requires explicit human authorization** — every time.
   Stage the diff, present it, wait for the literal word `commit`. The
   priority memory rule `feedback_commits_explicit_only.md` is operative:
   multi-step instructions like "fix everything" do not transitively
   authorize commits.
1. **`git push` is a separate authorization** from `git commit`. Wait
   for an explicit `push` reply.
1. **Never open a PR autonomously.** Do not run `gh pr create` unless
   explicitly asked. The operator opens the PR through the GitHub UI
   when they're ready.
1. **Before a non-trivial push, run `/codex:review`** (or equivalent
   second-pass review) and iterate to convergence. Reference:
   `feedback_codex_review_convergence.md`.
1. **Releases are operator-driven.** Do not bump `VERSION`, do not
   write `[X.Y.Z]` sections in `CHANGELOG.md`, do not tag, do not
   squash-merge `dev → main` — those are explicit-ask operations.

The per-package version-of-record is the repo-root `VERSION` file. The
CI workflow `version-sync.yml` (driven by
`scripts/ci/verify-version-sync.sh`) fails any PR or direct push to
`dev`/`main` whose sub-package versions drift from `VERSION`.

Full contributor reference, applicable to both humans and LLMs:
[`CONTRIBUTING.md`](CONTRIBUTING.md).

______________________________________________________________________

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
