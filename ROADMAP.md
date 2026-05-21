# ManyForge roadmap

This is the public direction document. It is not a commitment to delivery
dates or versions - items can move, get reordered, or be deferred as
integration work surfaces new constraints. See CHANGELOG.md for what is
actually planned for the next tag and what has shipped after tags exist.

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

**Goal.** Ship the third execution lane (`real-robot`) alongside the
existing `manyforge-only` and `isaac-sim` lanes. The `commanded_sim` /
`observed_robot` source-mode split already exists in the kernel; what's
missing is a paired deployment YAML, qualified tolerances, and a
launcher scenario.

**Acceptance criteria.**

- `examples/<arm>_real_robot.deployment.yaml` for at least one supported
  hardware target (UR10e + Robotiq 2F-140 baseline).
- `execution_state_source_mode: observed_robot` qualified end-to-end
  (controller-feedback freshness, tracking-tolerance envelope,
  stop-completion timeout) per spec 240.
- Re-enable controller-owned path/goal tolerance aborts (Isaac preview
  disables them; real robot must keep them on).
- Launcher scenario `--lane real-robot` + an `isaac-sim`-style
  readiness gate that probes the live controller.
- Operator runbook for first power-on, e-stop integration, and
  measured stop-completion characterization.

## Fake-Controller Chaos Tests

**Goal.** Cover the stop/reset race cases listed in spec 220 + the open
points: stop before goal acceptance, stop before first feedback, stale
goal result, controller never returns a result, repeated stop/reset
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

**Goal.** Move two assistant modes from preview to qualified:

- `query` mode — read-only inspection. Already validated; needs CI gates.
- `composer-assistant` mode — scene + behavior-tree authoring with
  proposal-then-review semantics. Validated on the smoke corpus
  (iter-32 production recipe: 51/66 = 77.3%); needs hardening before
  it's the recommended path for new operators.

**Acceptance criteria.**

- Smoke-corpus regression suite running in CI on every release.
- `inspect_isaac_scene` tool wired into the Isaac+AI scenario
  end-to-end (the scenario exists; the wiring needs a live-run
  validation pass).
- Bridge-fired `/compact` cadence tunable per deployment (currently
  pinned at N=2 for OpenClaw).

## Observability And Supervisability

**Goal.** Promote the existing tracing/heartbeat surfaces from
"present" to "production-supervisable."

**Acceptance criteria.**

- External-watchdog policy that consumes
  `/api/infra/heartbeat`: an off-the-shelf example wired to either
  systemd-watchdog (real robot) or an Isaac stop hook
  (`commanded_sim`).
- Failure-archive contents extended with: executing trajectory,
  rolling observed/predicted samples, controller result code/string
  as structured fields, deployment hash, program/artifact hash,
  measured capability profile.
- Cumulative observed-vs-predicted drift diagnostic as a maintenance
  signal (deployment-declared rolling window + threshold).
- Operational logging spec (spec 520) lands in production deployments
  including assistant-mode audit envelopes.

## Fine-Tuning And Agentic Loop

**Goal.** Use NemoClaw-Thor's JP7.2 (Q2 2026) SBSA transition to enable
SDFT/QAT-based fine-tuning of the assistant lane on operator-edited
program corpora.

**Acceptance criteria depend on Thor's SBSA milestone. Tracking link**:
NemoClaw-Thor ROADMAP §"Thor SBSA + fine-tuning".

## Production Readiness Bar

The bar for calling ManyForge production-ready:

1. All three execution lanes (manyforge-only, isaac-sim, real-robot)
   ship with qualified tolerances and an external-watchdog example.
2. Smoke-corpus + chaos tests are mandatory CI gates on every release.
3. The assistant `composer-assistant` mode hits ≥85% on the smoke
   corpus with the production recipe (currently 77.3%).
4. Public-facing APIs (kernel state surfaces, deployment YAML schema,
   assistant-provider contract) are versioned per spec 430 §schema and stable
   enough that breaking changes are explicit major-release decisions, not
   silent patch-level behavior shifts.
5. Spec ↔ implementation drift is automated-checked (the existing
   `check-private-spec-boundary.sh` is the boundary check; an
   additional CI lane verifying the assistant-bridge wire fields
   match `docs/reference/ASSISTANT_BACKEND_CONTRACT.md` would close
   this).

## Out of scope / explicitly deferred

These came up during the launcher pass but are not currently on the roadmap:

- **Per-package independent SemVer.** We chose unified-repo SemVer
  for the initial public line. If sub-packages start releasing on independent
  cadences (e.g. `manyforge_core` ships a patch faster than the rest), the
  model can split in a later major release decision.
- **Single-service hot-restart verbs** (`restart-bridge`,
  `reload-composer`). Deprecated in favor of the unified
  `scripts/launch.sh restart`. If iteration velocity loss is
  meaningful, they can be added back as per-service verbs later.
- **`Code of Conduct`**. Deferred until the project has external
  contributors. Adding it later is a single-PR change.

## Contributing to the roadmap

Roadmap items are not "claimed" — multiple contributors can work in
parallel on the same milestone. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the branch + PR workflow. If a
proposed change wants a new milestone added here, open an issue with
the `roadmap` label to discuss before opening the PR.
