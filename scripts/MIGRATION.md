# Demo-script migration notes

The seven `scripts/demo-*.sh` scripts are now thin shims that delegate to
`scripts/launch.sh` (the unified launcher). This document categorises
every demo-script behaviour into four buckets so operators with existing
runbooks know exactly what to expect:

1. **Preserved verbatim** — same env var, same effect.
2. **Preserved through aliases** — old scenario names still work with a
   one-time deprecation warning.
3. **Replaced** — behaviour exists under a different name / shape; the
   correct new invocation is listed.
4. **Intentionally removed** — the per-demo CLI flags that were too
   specific to migrate cleanly.

> **See also:**
> [`docs/operations/LAUNCHER_QUICKSTART.md`](../docs/operations/LAUNCHER_QUICKSTART.md)
> — the operator-facing smoke-test recipe + startup-contract reference.
> Keep both files in sync when launcher behaviour changes.

## 1. Preserved verbatim

These env vars work in the unified launcher exactly as they did in the
old demos:

- **Image / Docker control**: `REBUILD={auto|true|false}`,
  `STOP_KNOWN_CONFLICTS={true|false}`, `DROP_CACHES={true|false}`,
  `PRESERVE_OPENSHELL`. `STOP_KNOWN_CONFLICTS` now defaults to `false`;
  normal `start` fails when launcher-managed components are already running.
- **Composer / HMI endpoints**: `COMPOSER_URL` (full URL —
  `http://HOST:PORT`, no `/api/` suffix), `HMI_URL`.
- **Composer / HMI ports** (new — host-network aware):
  `COMPOSER_PORT` (default 9000), `HMI_PORT` (default 8081). The
  launcher passes these into the dev container as env vars; the
  inline Composer process reads them. Preflight honours the resolved
  values and now treats already-running launcher-managed components as
  conflicts; use `scripts/launch.sh stop` or `scripts/launch.sh restart`
  before starting again.
- **Isaac auto-load escape hatch**: `LOAD_ARTIFACTS={true|false}`.
  Setting `LOAD_ARTIFACTS=false` brings up the Isaac compose
  services without the supervisor auto-loading the program (operator
  drives the load step manually). Works through the launcher AND
  through raw `docker compose --profile ros-full up` —
  docker-compose.yaml falls back to `LOAD_ARTIFACTS` when
  `SUPERVISOR_AUTO_LOAD` is unset.
- **Isaac ROS surface**: `ROS_DISCOVERY_TIMEOUT_S`, `ARM_ACTION`,
  `GRIPPER_ACTION`, `ISAAC_JOINT_STATE_TOPIC`,
  `ARM_CONTROLLER_STATE_TOPIC`, `ARM_JOINT_STATE_TOPIC`,
  `TOPIC_BASED_PKG`.
- **Assistant lane control**: `MODEL_PROFILE`, `ASSISTANT_PROVIDER`,
  `START_VLLM_PROXY`, `VLLM_PORT`, `UPSTREAM_BASE_URL`,
  `OPENCLAW_ASSISTANT_COMPACT_EVERY_N`,
  `OPENCLAW_PROXY_OVERRIDE_MAX_TOKENS`,
  `ASSISTANT_TIMEOUT_S`, `ASSISTANT_MAX_TURNS`,
  `NEMOCLAW_THOR`.
- **Container names** (only set these if you know what you're doing):
  `COMPOSER_CONTAINER`, `VLLM_CONTAINER`.
- **Live progress display** during waits — the old
  `demo-assistant-known-good.sh` showed elapsed time + last 3 lines of
  container logs while polling. The unified launcher does this for
  every wait (`wait_for_composer`, `wait_for_hmi`, supervisor grace,
  all `smoke`-side probes). Set `MANYFORGE_PROGRESS_MODE=plain` to
  force the non-TTY codepath (no ANSI, periodic reprints every 30 s)
  for CI / log capture.

## 2. Preserved through aliases

Scenario names were renamed to descriptive canonical forms that
include the robot model and workflow. Every previous short name still
resolves through an alias table in `scripts/lib/scenarios.sh`, with a
**one-time deprecation warning per launch invocation** pointing at the
canonical name. The launcher's review-gate summary and the "to
reproduce non-interactively" hint also canonicalise the chosen name,
so copy-pasting the launcher's own output never propagates the old
alias forward.

| Deprecated alias | Canonical name |
|---|---|
| `pick-place` | `ur5-pick-place` |
| `interrupt` | `ur5-interrupt-pick-place` |
| `ur10e` | `ur10e-pick-place` |
| `shape-pick` | `ur5-semantic-shape-pick` |
| `vision-pose` | `ur5-vision-pose-pick` |
| `assistant-authoring` | `ur10e-scene-authoring` |
| `isaac-light` | `isaac-ur10e-light-scene` |
| `isaac-assistant` | `isaac-ur10e-light-scene-assistant` |

The new `ur5-simple-pick-place` scenario (from `demo.sh --simple`) was
introduced canonical-from-day-one and has no alias entry.

Old runbooks keep working unchanged. Operators see the warning, and
the launcher hint they copy points at the canonical name — so
migration happens passively as scripts get touched.

## 3. Replaced

Old behaviour preserved under a different shape — the table below maps
each old workflow to its current correct command:

| Old workflow | Correct replacement |
|---|---|
| `./scripts/demo.sh --simple` | Same flag, same effect (3-step test fixture). The shim strips `--simple` from any position in `$@` and forwards `--scenario ur5-simple-pick-place`. No env-var migration needed |
| `./scripts/demo-shape-pick.sh --api-port 9010` | `COMPOSER_PORT=9010 ./scripts/demo-shape-pick.sh start` (the old `--api-port` was undocumented as a port number, not a URL fragment; the new env-var path is what every shim respects) |
| `./scripts/demo-shape-pick.sh --hmi-port 8090` | `HMI_PORT=8090 ./scripts/demo-shape-pick.sh start` |
| `./scripts/demo-assistant-known-good.sh restart-bridge` | `./scripts/demo-assistant-known-good.sh restart` (full restart — see "Partial-restart workflow" below for the kill-and-`start` recipe if iteration velocity matters) |
| `./scripts/demo-assistant-known-good.sh reload-composer` / `restart-composer` | `./scripts/demo-assistant-known-good.sh restart` |
| `./scripts/demo-isaac-ros2-control.sh load` / `recover` | `./scripts/demo-isaac-ros2-control.sh start` (idempotent — re-runs the program/deployment load via `isaac_demo_supervisor`) |
| **Strict ROS-readiness gating** on Isaac lane start | Use `./scripts/launch.sh smoke` (or `./scripts/demo-isaac-ros2-control.sh smoke`) instead of `start`. The old script's `start` blocked synchronously on `/isaac_joint_states` + FJT + gripper + controller_state; the new launcher's `start` returns once Composer can report runtime status, and the `isaac_demo_supervisor` row in the popup tracks external readiness. CI scripts that need the strict gate should switch from `... start` to `... smoke` |
| **Strict vLLM-readiness gating** on assistant lane start | Same pattern: `start` checks that the vLLM container, proxy, and bridge launched and didn't immediately exit (process liveness); the `assistant_supervisor` row in the popup tracks full readiness (`/v1/models` ready, served model id matches, bridge `/healthz`). Use `./scripts/launch.sh smoke` to block synchronously on the strict probes |
| `HEALTH_TIMEOUT_S=NN` (single ceiling for Composer health) | Replaced by per-service env vars, split by whether they apply during `start` or only during `smoke`. **Start-side** (default): `COMPOSER_READY_TIMEOUT_S=600` (Composer health, blocking), `HMI_PORT_READY_TIMEOUT_S=30` (non-fatal, runs after program load), `VLLM_LIVENESS_GRACE_S=5` / `PROXY_LIVENESS_GRACE_S=2` / `BRIDGE_LIVENESS_GRACE_S=5` (process-up checks), `ASSISTANT_SUPERVISOR_GRACE_S=3`. **Smoke-side** (strict readiness, default): `VLLM_MODEL_READY_TIMEOUT_S=900`, `VLLM_PROXY_READY_TIMEOUT_S=30`, `ASSISTANT_BRIDGE_READY_TIMEOUT_S=60`, `ROS_DISCOVERY_TIMEOUT_S=45`. The old `HEALTH_TIMEOUT_S` still works as an alias for `COMPOSER_READY_TIMEOUT_S` (for one release cycle, with a one-time deprecation warning). See `scripts/launch.sh --help` for the same table. |

### Restart workflow (replaces `restart-bridge` / `reload-composer`)

The unified launcher's `start` path is intentionally strict: if
launcher-managed Composer, vLLM, bridge, supervisor, or Isaac services
are already running, preflight fails and tells the operator to stop or
restart first. This avoids accidentally attaching a new scenario to a
stale process graph.

```bash
# Old: ./scripts/demo-assistant-known-good.sh restart-bridge
# New: restart the selected stack coherently
scripts/launch.sh restart --lane manyforge-only --assistant on \
  --scenario ur10e-scene-authoring --non-interactive --yes
```

## 4. Intentionally removed

These per-demo CLI flags from the old shape-pick / vision-pose scripts
are not parsed by the launcher. The recommended migration is either an
env-var override (where one applies) or a custom deployment YAML.

| Old script | Removed flag | Migration path |
|---|---|---|
| `demo-shape-pick.sh` | `--requested-shape SHAPE` | Edit `examples/semantic_shape_pick.deployment.yaml` (or supply a custom `--deployment PATH` to the launcher) |
| `demo-shape-pick.sh` | `--selection-request TEXT` | Same — deployment YAML field |
| `demo-shape-pick.sh` | `--selection-image-mode {text\|inline\|http}` | Same — only `text` is the validated mode anyway |
| `demo-shape-pick.sh` | `--model-base-url URL` | `UPSTREAM_BASE_URL=URL` |
| `demo-shape-pick.sh` | `--model-id ID` | `MODEL_PROFILE=ID` |
| `demo-shape-pick.sh` | `--rebuild` | `REBUILD=true` |
| `demo-shape-pick.sh` | `--skip-bootstrap` | `REBUILD=false` |
| `demo-vision-pose.sh` | _(same flag set)_ | _(same replacements)_ |

If you maintained CI scripts that pass these flags, update them to set
the env var or to call the unified launcher with a custom deployment:

```bash
# old
./scripts/demo-shape-pick.sh --requested-shape cube --rebuild

# new
REBUILD=true \
  scripts/launch.sh --lane manyforge-only --assistant on \
    --scenario ur5-semantic-shape-pick --non-interactive --yes start
# then edit examples/semantic_shape_pick.deployment.yaml — or write a
# small variant with requestedShape=cube and pass it via --deployment
```

## Deferred (not yet decided)

- **Single-service hot-restart verbs** (`restart-bridge`,
  `reload-composer`). Currently mapped to full `restart`. If the
  iteration-velocity loss matters in practice, a follow-up can add
  `restart-bridge` / `restart-composer` verbs to the unified launcher
  that key off `ASSISTANT` / `LANE`.
