# Launcher quick start

This is the operator's recipe for bringing up the ManyForge stack with
the unified launcher (`scripts/launch.sh`). Two flows are covered:

1. **Interactive wizard** — the primary way to use the launcher. No
   flags needed; a 7-step menu walks you through lane, assistant, and
   scenario selection (mirrors the NemoClaw onboard wizard).
2. **Non-interactive / scripted** — flag-driven invocation for CI,
   reproducible verification, and the smoke-test matrix.

> **Maintenance rule** — if you modify any of the following, update
> this file in the same commit so operators don't end up with stale
> instructions:
>
> - `scripts/launch.sh` (verbs, flags, wizard order, prompts)
> - `scripts/lib/*.sh` (any helper used by the launcher)
> - `scripts/lib/scenarios.sh` (the scenario table)
> - `scripts/demo-*.sh` (the seven shims)
>
> Companion docs that point operators at this file: [`README.md`](../../README.md)
> Quick Start, [`AGENTS.md`](../../AGENTS.md) Runtime section,
> [`docs/operations/STACK_SETUP.md`](STACK_SETUP.md) Primary Scripts,
> [`scripts/MIGRATION.md`](../../scripts/MIGRATION.md) footer.

Run everything from the **manyforge repo root** on the host. The
launcher orchestrates Docker for you — you do not need to be inside a
container.

```bash
cd ~/workspaces/dev_ws/src/manyforge   # adjust to your checkout path
```

## Prerequisite: first-time install already done

`scripts/launch.sh` is a runtime orchestrator, not a clean-machine installer.
Before using it for the full assistant stack, the host must already have Docker
and the NVIDIA runtime, the ManyForge image, the local model-serving toolchain,
and a first-time NemoClaw sandbox created with `nemoclaw onboard`.

For a fresh Thor/workstation, follow
[`STACK_SETUP.md`](STACK_SETUP.md#clean-machine-install-order). For Jetson Orin
AGX, follow [`ORIN_STACK_SETUP.md`](ORIN_STACK_SETUP.md) first. After those
one-time prerequisites exist, this launcher is the supported repeatable way to
start, stop, restart, and smoke the stack.

## What the launcher offers

Two orthogonal selectors:

- **Lane** = which execution path to bring up:
  - `manyforge-only` — Composer + native execution backend, no ROS.
  - `isaac-sim` — Isaac Sim commanded-sim via `ros2_control` /
    `FollowJointTrajectory`. Requires Isaac Sim publishing
    `/isaac_joint_states` on the host.
- **Assistant** = whether to bring up the AI stack:
  - `off` — no vLLM / OpenClaw / bridge / supervisor; AI button
    hidden in Composer UI.
  - `on` — full assistant stack (vLLM + optional proxy + OpenClaw or
    nemoclaw bridge + supervisor). AI button visible if the
    deployment declares `assistant.enabled: true`.

Plus a named **scenario** selecting the program + deployment YAML
pair. Nine scenarios today (including a 3-step `ur5-simple-pick-place`
test fixture, the new `--simple` replacement); four of them cover the
lane × assistant matrix.

---

## Startup contract — what `start` blocks on vs what the popup reports

`./scripts/launch.sh start` blocks on the services it manages:

- Composer API health (`/api/health`)
- Runtime-status endpoint (`/api/runtime/status`)
- Program/deployment load (manyforge-only) or supervisor handoff (isaac-sim)
- Supervisor processes — `assistant_supervisor`, `isaac_demo_supervisor`
- vLLM / proxy / bridge containers + PIDs **(liveness only — did they
  exit immediately?)**

It does **NOT** block on external readiness:

- Isaac Sim publishing `/isaac_joint_states`
- FJT / gripper action servers
- vLLM model fully loaded
- OpenClaw gateway ready
- Bridge full `/healthz` (it depends on the model)

External readiness appears in the **Composer status popup** via the
supervisors, with status `waiting` until the dependency comes online,
then `ready`. Use the popup as the source of truth after `start`
returns.

For strict end-to-end verification — CI gate, pre-cell-test sanity —
use `./scripts/launch.sh smoke`. That **does** block on every external
probe (Isaac topics, action servers, controller state, vLLM `/models`,
bridge `/healthz`) and fails the run if any one of them doesn't come
up within its timeout. `smoke` is the heir of the old
`demo-isaac-ros2-control.sh start` readiness gate.

---

## Interactive wizard (primary use)

This is the way you should use the launcher day-to-day. Just run it
with no arguments:

```bash
./scripts/launch.sh
```

The launcher detects that it's running in a terminal and walks you
through a 7-step wizard (same shape as
[NemoClaw's onboard wizard](https://github.com/NVIDIA/NemoClaw)):

```
==> [1/7] Choose lane

  Execution lane
    1) manyforge-only
    2) isaac-sim

  Choose [1]: _

==> [2/7] Activate AI assistant?

  Activate AI assistant
    1) on
    2) off

  Choose [2]: _

==> [3/7] Pick scenario

  Scenario
    1) ur5-pick-place
    2) ur5-simple-pick-place
    3) ur10e-pick-place

  Choose [1]: _

  --- review gate ---
  Plan:
    Lane:        manyforge-only
    Assistant:   off
    Scenario:    ur5-pick-place
      Program:     /workspace/examples/pick_and_place.program.yaml
      Deployment:  /workspace/examples/pick_and_place.deployment.yaml
    Compose:     dev container (detached), no profile
    Supervisors: (none required for lane)
    Endpoints:   Composer http://127.0.0.1:9000   Viewer native Composer panel

  Continue? [Y/n] _
```

After each step the chosen value is echoed (`✓ Lane: manyforge-only`)
so you have an audit trail in the terminal. The review gate at the
bottom is your last chance to back out before anything starts.

After you confirm:

- `[4/7]` runs preflight checks (Docker reachable, ports free, image
  present).
- `[5/7]` brings up the lane-specific compose services or detached
  Composer container.
- `[6/7]` waits for Composer health and loads the program /
  deployment.
- `[7/7]` brings up the AI stack (only if assistant=on): vLLM → proxy
  → OpenClaw provisioning → bridge → supervisor.

When all seven steps are green, the launcher prints the equivalent
non-interactive invocation it just ran — copy it into a script if you
want to reproduce this exact configuration later:

```
  To reproduce non-interactively:
    scripts/launch.sh --lane manyforge-only --assistant off \
      --scenario ur5-pick-place --non-interactive --yes
```

### Stop / status / restart (interactive)

The launcher accepts verbs anywhere on the command line:

```bash
./scripts/launch.sh status      # what's running right now
./scripts/launch.sh stop        # tear down everything launcher-managed
./scripts/launch.sh restart     # stop + start, prompts again from scratch
```

`restart` re-runs the wizard from step 1; if you want to restart with
the same selection, supply the flags from the "To reproduce
non-interactively" hint the launcher gave you.

---

## Non-interactive / scripted use

This is the path for CI, automated smoke tests, and reproducible
configurations. Bypass any of the wizard's prompts by passing the
matching flag.

### Selection flags

| Flag | Replaces wizard step |
|---|---|
| `--lane manyforge-only` / `--lane isaac-sim` | `[1/7]` |
| `--assistant on` / `--assistant off` | `[2/7]` |
| `--scenario NAME` | `[3/7]` (use `--list-scenarios` to see all nine; deprecated aliases — `pick-place`, `ur10e`, `shape-pick`, `vision-pose`, `assistant-authoring`, `isaac-light`, `isaac-assistant` — also resolve with a one-time deprecation warning per launch. Includes `isaac-ur10e-light-scene-collision` for testing runtime collision-fault detection with a popup object.) |
| `--program PATH --deployment PATH` | Free-form alternative to `--scenario` |
| `--non-interactive` (env: `MANYFORGE_NON_INTERACTIVE=1`) | Skip all prompts; any unset selection takes its default value |
| `--yes` / `-y` (env: `MANYFORGE_ASSUME_YES=1`) | Skip the review gate confirmation |

Supplying all three of `--lane / --assistant / --scenario` plus `--yes`
makes the run fully non-interactive (no prompts, no review gate):

```bash
./scripts/launch.sh --lane manyforge-only --assistant off \
    --scenario ur5-pick-place --yes start
```

`--non-interactive` is the equivalent of `--yes` plus
"use the default selection for any flag that's missing" — useful for
CI where you want a deterministic outcome even if a future scenario
gets added.

### Verbs

`start` (default), `stop`, `status`, `restart`, `smoke`, `preflight`.
Same meaning in interactive and non-interactive mode. `preflight` is
the most useful CI verb because it runs the decision phase + system
checks but exits before starting anything.

---

## Smoke-test matrix (verify all four combinations)

Use this when validating the launcher itself — after a refactor, a
new scenario addition, or first-run sanity on a new host. The
sequence is non-interactive on purpose so it's reproducible.

### Step 0 — Zero-side-effect sanity

```bash
./scripts/launch.sh --help
./scripts/launch.sh --list-scenarios
./scripts/ci/verify-version-sync.sh
```

If any of those error, fix that before going further. None of these
touches Docker.

### Step 1 — Preflight per combination

```bash
./scripts/launch.sh --lane manyforge-only --assistant off \
    --scenario ur5-pick-place --yes preflight

./scripts/launch.sh --lane manyforge-only --assistant on \
    --scenario ur10e-scene-authoring --yes preflight

./scripts/launch.sh --lane isaac-sim --assistant off \
    --scenario isaac-ur10e-light-scene --yes preflight

./scripts/launch.sh --lane isaac-sim --assistant on \
    --scenario isaac-ur10e-light-scene-assistant --yes preflight
```

Each should end with `✓ Preflight ok; not bringing services up`. If a
port is busy and the launcher does not recognize the owner, it
refuses with `! Port N is in use by an unmanaged listener` — fix
before continuing.

### Step 2 — Smallest end-to-end (~30 s)

```bash
./scripts/launch.sh --lane manyforge-only --assistant off \
    --scenario ur5-pick-place --yes start

# Browser:  http://127.0.0.1:9000 (Composer UI)
# Verify:   status pill green "Ready", 5 component rows in popup,
#           AI button HIDDEN.

./scripts/launch.sh --yes stop
```

### Step 3 — The other three combinations

Same shape, just swap the selectors. Between tests, always `stop`
first so port conflicts don't cause confusing errors:

```bash
# manyforge-only + AI on
./scripts/launch.sh --lane manyforge-only --assistant on \
    --scenario ur10e-scene-authoring --yes start
# popup: core 5 + assistant_supervisor rows (vllm, vllm_proxy,
#        openclaw_gateway, assistant_bridge). AI button VISIBLE.
./scripts/launch.sh --yes stop

# isaac-sim + no AI
./scripts/launch.sh --lane isaac-sim --assistant off \
    --scenario isaac-ur10e-light-scene --yes start
# popup: core 5 + isaac_demo_supervisor rows. AI button HIDDEN.
./scripts/launch.sh --yes stop

# isaac-sim + AI on
./scripts/launch.sh --lane isaac-sim --assistant on \
    --scenario isaac-ur10e-light-scene-assistant --yes start
# popup: core 5 + isaac rows + assistant rows. AI button VISIBLE.
./scripts/launch.sh --yes stop
```

### Step 4 — Shim parity check

Verify the backward-compatibility shims still route correctly:

```bash
./scripts/demo.sh start                              # ≡ manyforge-only + ur5-pick-place
./scripts/demo.sh --simple start                     # ≡ manyforge-only + ur5-simple-pick-place
./scripts/demo.sh stop

./scripts/demo-ur10e-robotiq-pick-place.sh start     # ≡ manyforge-only + ur10e-pick-place
./scripts/demo-ur10e-robotiq-pick-place.sh stop

./scripts/demo-shape-pick.sh start                   # ≡ manyforge-only + assistant + ur5-semantic-shape-pick
./scripts/demo-shape-pick.sh stop

./scripts/demo-vision-pose.sh start                  # ≡ manyforge-only + assistant + ur5-vision-pose-pick
./scripts/demo-vision-pose.sh stop

./scripts/demo-isaac-ros2-control.sh start           # ≡ isaac-sim + isaac-ur10e-light-scene
./scripts/demo-isaac-ros2-control.sh stop

./scripts/demo-assistant-known-good.sh start         # ≡ manyforge-only + ur10e-scene-authoring
./scripts/demo-assistant-known-good.sh stop
```

Each should emit the unified wizard banners (`==> [1/7] Choose lane`,
`==> [2/7] Activate AI assistant?`, …) — confirming the shim is
routing through `launch.sh` rather than the old inline code.

See [`scripts/MIGRATION.md`](../../scripts/MIGRATION.md) for the full
list of legacy flags / verbs that have been dropped or remapped.

---

## Status + recovery commands (useful any time)

```bash
./scripts/launch.sh status                  # what's running right now
./scripts/launch.sh stop                    # tear down launcher-managed services
docker ps | grep -E '(manyforge|isaac|composer-ros)'
ss -ltnp | grep -E ':(9000|8081|8000|8050|8100|8200)'
```

## Likely first-run gotchas

Bring-up-specific cases below. For cross-cutting runtime failures
(collision rejection, motion interrupted, assistant fallback to mock,
ros2_control discovery) see [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md).
For where logs and observability endpoints live, see
[`OBSERVABILITY.md`](OBSERVABILITY.md).

- **`manyforge-dev:latest` not built** → preflight fails with a clear
  error. Run `./scripts/setup-docker.sh` first.
- **Composer takes minutes on first launch** → the dev container does a
  cold-cache C++ + roboplan build inside on first run; `start` shows
  live progress with the last few build-log lines. Default budget is
  600 s (10 min); on a slow host or a fresh image override with
  `COMPOSER_READY_TIMEOUT_S=1200` (or higher).
- **vLLM model load is slow** (for `--assistant on`) → `start` only
  checks that the vLLM container launched and didn't immediately exit.
  Full model readiness — including FlashInfer JIT compile (~60+ min
  cold) — is reported by the assistant supervisor into the Composer
  status popup. To gate on it explicitly (CI, pre-cell-test), use
  `./scripts/launch.sh smoke` which runs the strict probe. Override
  the strict-probe budget with `VLLM_MODEL_READY_TIMEOUT_S=NN`.
- **Isaac Sim not running** (for `--lane isaac-sim`) → `start` returns
  green; `isaac_demo_supervisor` reports `/isaac_joint_states`,
  FJT/gripper action readiness as `waiting` until Isaac comes online,
  then `ready`. Watch the Composer popup. Use `smoke` to block on these
  with strict readiness (override budget with `ROS_DISCOVERY_TIMEOUT_S=NN`).
- **Port 9000 busy** from a stale prior run →
  `./scripts/launch.sh stop` clears the `manyforge-e2e-composer`
  container; a leftover unmanaged process needs `docker ps` + `docker
  rm -f` or a process kill. To use different ports, set
  `COMPOSER_PORT=NNNN` (preflight checks the resolved value;
  `expected_port_owner` matches it).
- **CI/log-capture wants plain output** → live progress redraw uses
  ANSI cursor-up. For CI logs, set `MANYFORGE_PROGRESS_MODE=plain`;
  the helper reprints the full status block every 30 s instead of
  redrawing.
- **Collision validation worker sizing** → authoritative trajectory
  validation defaults to `MANYFORGE_VALIDATION_COLLISION_WORKERS=4`.
  This is the baseline for Jetson Orin Nano-class and larger systems.
  Use `MANYFORGE_VALIDATION_COLLISION_WORKERS=1` or `2` only when a
  deployment's controller / observer latency qualification shows that
  CPU headroom is too tight.
- **`HEALTH_TIMEOUT_S` from old runbooks** → deprecated alias for
  `COMPOSER_READY_TIMEOUT_S`. Both work for one release; you'll see a
  one-time warning per launch. Switch your runbooks to the per-service
  vars listed above.
- **Unified memory not freed after `stop` on Thor** → the launcher
  runs `sync` + `echo 3 > /proc/sys/vm/drop_caches` at the end of
  `stop_assistant_stack`, but this requires passwordless sudo. If
  you see `drop_caches needs sudo but sudo would prompt for a
  password` after stop, add a sudoers rule (see
  `docs/operations/ASSISTANT_E2E_COOKBOOK.md` "Stop and free memory"
  for the exact rule) or set `DROP_CACHES=false` to suppress the
  attempt entirely.

## Jetson Orin AGX — Gemma 4 12B (GGUF) assistant

> **Bringing up the whole stack on a fresh Orin?** This section covers running
> the launcher once the stack is installed. For the end-to-end install (host
> storage → model serving → NemoClaw/OpenShell/OpenClaw toolchain → onboarding →
> composer build → run → smoke) start at the master index:
> [`ORIN_STACK_SETUP.md`](ORIN_STACK_SETUP.md).

The Thor assistant lane defaults to `MODEL_PROFILE=gemma4-12b-it-gguf`.
On a **Jetson Orin AGX Dev Kit (64 GB)** use the
`gemma4-12b-it-gguf-orin` sibling profile: the same Gemma 4 12B
(unsloth GGUF UD-Q4_K_XL) model served by llama.cpp with the Gemma 4
E2B GGUF as the speculative draft, on the Orin llama.cpp image. It
tracks the verified Thor serving recipe — 128k context, q8_0 KV cache
(model + draft) and `--flash-attn on` — so the two stay in lockstep;
only the image and cache mounts are Orin-specific. There is a turnkey
wrapper:

```bash
./scripts/launch-orin-gemma.sh            # interactive review gate, then start
./scripts/launch-orin-gemma.sh -y         # skip the review gate
./scripts/launch-orin-gemma.sh smoke      # start + strict assistant e2e smoke
./scripts/launch-orin-gemma.sh stop
```

It is a thin shim over `launch.sh` that pins `MODEL_PROFILE` and selects
`--lane manyforge-only --assistant on --scenario ur10e-scene-authoring`
(Gemma 4 GGUF is text-only — `--no-mmproj` — so the non-vision scenario
is the natural pairing). Override any flag by re-passing it. The
equivalent explicit invocation is:

```bash
MODEL_PROFILE=gemma4-12b-it-gguf-orin ./scripts/launch.sh \
    --lane manyforge-only --assistant on --scenario ur10e-scene-authoring start
```

**The NVMe mounts are load-bearing.** The `gemma4-12b-it-gguf-orin`
profile (defined in `NemoClaw-Thor/serving/{config,launch}.sh`) points the
llama.cpp caches at the ~1 TB NVMe so the ~7 GB GGUF + draft stay off the
small eMMC and persist across `docker run --rm`:

| Host (NVMe) | Container |
| --- | --- |
| `/mnt/nova_ssd/hf-cache-orin` | `/data/models/huggingface` and `/root/.cache/huggingface` |
| `/mnt/nova_ssd/llama-cpp-cache` | `/root/.cache/llama.cpp` |

This mirrors the operator's tested standalone `docker run` and the storage
layout in
[`isaac_ros_custom_bringup/jetson_orin_storage/README.md`](../../../isaac_ros_custom_bringup/jetson_orin_storage/README.md)
(`/mnt/nova_ssd` is the NVMe, made persistent in `/etc/fstab` and
`chown`'d to the user). If the sibling checkout is missing, use
`https://github.com/pastoriomarco/isaac_ros_custom_bringup` or the accepted
local alternate
`${HOME}/workspaces/isaac_ros-dev/src/isaac_ros_custom_bringup`. Override the
host roots with `THOR_HF_CACHE_DIR`
and `THOR_LLAMACPP_CACHE_DIR_HOST` if your NVMe is mounted elsewhere. The
first `start` downloads the GGUFs (several GB); subsequent starts reuse
the NVMe cache. As with all assistant profiles, `start` returns once the
container is live — model readiness is reported into the Composer popup,
and `smoke` blocks on it strictly.

---

## Where to go from here

- **Run a real cycle**: open Composer in a browser, hit **Run** on
  the loaded program, watch the behavior tree execute.
- **Bring up the assistant stack on Thor**: see
  [`docs/operations/ASSISTANT_E2E_COOKBOOK.md`](ASSISTANT_E2E_COOKBOOK.md).
- **Bring up the assistant stack on Orin AGX**: use
  `./scripts/launch-orin-gemma.sh` (see the Jetson Orin section above).
- **Stack-level operational concerns**:
  [`docs/operations/STACK_SETUP.md`](STACK_SETUP.md).
- **Add a new scenario**: edit
  [`scripts/lib/scenarios.sh`](../../scripts/lib/scenarios.sh) (one
  pipe-delimited row per scenario), then update the "Interactive
  wizard" sample above (the scenario list it shows) AND the
  smoke-test matrix in Step 3 of this file to include the new
  combination.
- **Add a new lane or assistant value**: bigger surgery —
  [`scripts/launch.sh`](../../scripts/launch.sh) wizard steps,
  [`scripts/lib/compose.sh`](../../scripts/lib/compose.sh) profile
  mapping, [`scripts/lib/preflight.sh`](../../scripts/lib/preflight.sh)
  port owner table. Update the full Interactive + Step 0-5 matrix in
  this file when done.
