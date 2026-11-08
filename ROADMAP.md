# ManyForge roadmap

> **Status (2026-11-02):** `v0.1.0` is tagged on the old `main` (`8ae05265`).
> `0.2.0` is in validation: component model v2, every lane on ROS 2 Lyrical
> with `ros2_control` and the stock joint trajectory controller (Mock, Isaac
> Sim attach-only, Real for UFACTORY xArm-family robots), and RoboPlan 0.8
> planning. See [CHANGELOG.md](CHANGELOG.md). Physical execution is not
> qualified. The workstreams below are planned work; where a line names
> something that exists, it says so.

This is the public product-direction backlog. It is not a commitment to
delivery dates or versions: items can move, be reordered, or be deferred as
integration work surfaces new constraints. See CHANGELOG.md for what each
tag ships and what the next one adds.

The roadmap is structured by **what** before **when**: the workstreams below
describe outcomes the project is working toward, in rough priority order. Each
workstream names the work it depends on and the criteria for "done"; none of
them is pinned to a specific future version.

## Companion roadmap

NemoClaw-Thor publishes its own roadmap focused on serving + sandboxing:
the model server (vLLM container generations), the OpenClaw bridge lane,
and the Thor-hardware specifics. See
[NemoClaw-Thor ROADMAP](https://github.com/pastoriomarco/NemoClaw-Thor/blob/main/ROADMAP.md).
The two roadmaps coordinate compatibility constraints; check both before
planning a release that crosses the boundary.

## Real-Robot Execution Lane

**Goal.** Qualify the Real lane alongside the Mock and Isaac lanes. In
0.2.0 each component names its `lane` (`mock`, `isaac`, `real`); the Real
lane drives UFACTORY xArm-family robots through the vendor driver (Lite 6
measured on hardware, `real_lite6_*` Scenarios). What's missing is
qualified tolerances, a second hardware target, and the runbook items
below.

**Acceptance criteria.**

- A `real_*` Scenario for at least one further supported hardware target
  (UR10e + Robotiq 2F-140 baseline; there is no real UR lane yet).
- `execution_state_source_mode: observed_robot` qualified end-to-end
  (controller-feedback freshness, tracking-tolerance envelope,
  stop-completion timeout).
- Re-enable controller-owned path/goal tolerance aborts (Isaac preview
  disables them; real robot must keep them on).
- A readiness gate that probes the live controller before Execute, as
  the Isaac lane's attach checks do.
- Operator runbook for first power-on, e-stop integration, and
  measured stop-completion characterization.

## Fake-Controller Chaos Tests

**Goal.** Cover the stop/reset race cases: stop before goal acceptance,
stop before first feedback, stale goal result, controller never returns a result, repeated stop/reset
convergence, gripper action server hang.

**Acceptance criteria.**

- A test rig that injects each failure pattern into the existing
  `ros2_control` execution backend.
- All cases either complete in bounded time or escalate to
  `InterventionRequired` with `resync_required=true` — never an
  unbounded wait.
- ThreadSanitizer coverage extended from `manyforge_core` to the
  RoboPlan/ROS-backed runtime tests.

## Assistant Graduations

**Goal.** Move the `composer-assistant` mode from preview to qualified.
It authors the Scene and the behavior tree; its edits land in the Program
Draft, and the operator applies or discards them. It was validated on the
smoke corpus before the 0.2.0 rework (iter-32 production recipe: 51/66 =
77.3%) and needs hardening before it's the recommended path for new
operators. The read-only `query` mode was removed; the assistant uses an
`ai_model` Deployment component as its endpoint.

The read-only `inspect_isaac_scene` tool is in the `composer-assistant`
catalog of the `sim_ur10e_assistant_scene_authoring` Scenario. It was
exercised through the live ROS/Isaac path before the 0.2.0 rework; that is
an integration baseline, not a claim that every current model/profile
combination is release-qualified.

**Acceptance criteria.**

- Smoke-corpus regression suite running in CI on every release.
- Retain a current live Isaac + assistant-profile regression for
  `inspect_isaac_scene` and the proposal/review boundary.
- Bridge-fired `/compact` cadence tunable per deployment (currently
  pinned at N=2 for OpenClaw).

## Observability And Supervisability

**Goal.** Promote the tracing and health surfaces from "present" to
"production-supervisable."

**Acceptance criteria.**

- External-watchdog policy that consumes the cell's health
  (`GET /api/components`, `GET /api/cell/latch`; 0.1.0's
  `/api/infra/heartbeat` is not in 0.2.0): an off-the-shelf example wired
  to either systemd-watchdog (Real lane) or an Isaac stop hook (Isaac
  lane).
- Failure-archive contents extended with: executing trajectory,
  rolling observed/predicted samples, controller result code/string
  as structured fields, deployment hash, program/artifact hash,
  measured capability profile.
- Cumulative observed-vs-predicted drift diagnostic as a maintenance
  signal (deployment-declared rolling window + threshold).
- Operational logging lands in production deployments
  including assistant-mode audit envelopes.

## Fine-Tuning And Agentic Loop

**Goal.** Use NemoClaw-Thor's JP7.2 (Q2 2026) SBSA transition to enable
SDFT/QAT-based fine-tuning of the assistant lane on operator-edited
program corpora.

**Acceptance criteria depend on Thor's SBSA milestone. Tracking link**:
NemoClaw-Thor ROADMAP §"Thor SBSA + fine-tuning".

## Production Readiness Bar

The bar for calling ManyForge production-ready:

1. All three lanes (Mock, Isaac, Real) ship with qualified tolerances
   and an external-watchdog example.
1. Smoke-corpus + chaos tests are mandatory CI gates on every release.
1. The assistant `composer-assistant` mode hits ≥85% on the smoke
   corpus with the production recipe (77.3% before the 0.2.0 rework).
1. Public-facing APIs (kernel state surfaces, deployment YAML schema,
   assistant-provider contract) are versioned and stable
   enough that breaking changes are explicit major-release decisions, not
   silent patch-level behavior shifts.
1. Keep generated REST, MCP, assistant-provider, and viewer contracts plus
   cross-repository architecture documentation under freshness/convergence CI;
   extend owner-derived checks when a new public wire surface is introduced.

## Out of scope / explicitly deferred

These are not currently on the roadmap:

- **Per-package independent SemVer.** We chose unified-repo SemVer
  for the initial public line. If sub-packages start releasing on independent
  cadences (e.g. `manyforge_core` ships a patch faster than the rest), the
  model can split in a later major release decision.
- **`Code of Conduct`**. Deferred until the project has external
  contributors. Adding it later is a single-PR change.

## Contributing to the roadmap

Roadmap items are not "claimed" — multiple contributors can work in
parallel on the same milestone. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the branch + PR workflow. If a
proposed change wants a new milestone added here, open an issue with
the `roadmap` label to discuss before opening the PR.
