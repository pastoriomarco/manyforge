# Observability — Logs, Endpoints, Diagnostic Capture

> **Cutover status:** retained as a restoration and behavior reference. The
> command recipes below are not qualified on the current `dev` branch and
> must not be executed until the owning H0/N/I/R gate passes. Present-tense
> endpoint, process, and log-path descriptions below refer to the captured
> runtime unless explicitly identified as structural-only.

This doc inventories the observability surfaces an operator (human or LLM)
had in the captured runtime: where logs went, which REST endpoints
were queried for what state, and which scripts captured diagnostic
artifacts.

For symptom-to-cause mapping see
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md). For the full route reference,
see [`../reference/REST_API.md`](../reference/REST_API.md) (auto-generated
from the FastAPI app).

## Logs

ManyForge does **not** ship a unified log file by default. Composer
logs go to stderr (captured by Docker); the assistant bridge writes
both stderr and a JSONL audit file; kernel events live in memory and
are surfaced through REST.

### Composer backend

- **Where**: stderr only. The Composer backend's
  `manyforge_composer/backend/logging_config.py` configures a
  `JSONFormatter` writing single-line JSON to `sys.stderr`. There is no
  on-disk log file from Composer itself.
- **How to read**: when Composer runs in the launcher's container
  `manyforge-e2e-composer`, use `docker logs manyforge-e2e-composer`.
  For the ros-full / Isaac lane the service name is `composer-ros`:
  `docker compose logs --tail 100 composer-ros`.
- **What's in it**: structured JSON per request (path, status,
  duration_ms, client) plus any service-level INFO/WARNING/ERROR.
  `uvicorn.access` is quieted to WARNING by default.
- **Level**: INFO by default. Composer does not read a `LOG_LEVEL`
  env var today; raise verbosity by editing `logging_config.py` or
  by setting per-logger config in the running container.

### Assistant bridge

- **Where (stderr)**: the captured bridge blob
  `a2f89b26da46355412a83362d3e4bcedc38137cd` used
  `logging.basicConfig(level=INFO)` — plain (not JSON) format, stderr only.
  The captured supervisor blob
  `8c0ecee5ad79b553063c0f171740a3ccf617e8d7` read `LOG_LEVEL` (default
  INFO). Those live effects are absent from the current gated bridge and
  pure supervisor reducer.
- **Where (audit JSONL)**: the captured bridge blob's
  `BRIDGE_AUDIT_LOG` setting (lines 83–98). Its default path was
  `$XDG_STATE_HOME/manyforge/assistant-bridge-audit.log` (size-rotated). The launcher
  overrides to `${LOG_DIR}/known-good-bridge-audit.jsonl` — see below.
- **What's in the audit JSONL**: every assistant turn, fail-closed
  reject, stuck-loop hint/fail, and upstream-error event, with the
  envelope fields the contract requires. This is the canonical audit
  trail for assistant-lane debug.

### Launcher process logs

The launcher pins all bridge/supervisor/vLLM-proxy artifacts under
`$LOG_DIR` (default `/tmp/manyforge-assistant-e2e/`, set in
`scripts/lib/assistant.sh:56`). After `./scripts/launch.sh start --lane manyforge-only --assistant on` you can expect:

| File                                     | What it is                                         |
| ---------------------------------------- | -------------------------------------------------- |
| `$LOG_DIR/known-good-bridge.log`         | Bridge stdout/stderr capture                       |
| `$LOG_DIR/known-good-bridge.pid`         | Bridge PID for stop                                |
| `$LOG_DIR/known-good-bridge-audit.jsonl` | Bridge audit JSONL (per-turn records)              |
| `$LOG_DIR/assistant-supervisor.log`      | Supervisor stdout/stderr                           |
| `$LOG_DIR/assistant-supervisor.pid`      | Supervisor PID for stop                            |
| `$LOG_DIR/vllm-proxy.log`                | vLLM proxy stdout/stderr (text)                    |
| `$LOG_DIR/vllm-proxy.jsonl`              | vLLM proxy structured JSONL                        |
| `$LOG_DIR/vllm-proxy.pid`                | vLLM proxy PID for stop                            |
| `$LOG_DIR/launcher-smoke.json`           | Last `smoke` verb output (`scripts/launch.sh:616`) |

### Kernel events

Kernel events are **not persisted to disk**. They are buffered in
memory by `manyforge_composer/backend/event_recorder.py` and surfaced
through:

- `GET /api/kernel/events?since=N` — drains the queue since cursor N.
- `GET /api/execution/history` — same store, with rich filtering
  (`eventTypes`, `minSeverity`, `components`, `startNs`, etc.).
- `GET /api/execution/digest` — the high-level summary used by the
  Composer UI.
- `GET /api/execution/alarms` — current condition-backed alarms, source/
  manager health, and bounded lifecycle history.

There is no log-replay mechanism today; if you need to capture a
failure for offline analysis use `GET /api/debug/export?run_id=…`
(see below).

### What is **not** a runtime log

These directories exist at repo root and are easy to mistake for
runtime logs — they aren't:

- `log/` — `colcon build` output (ROS package compile logs). Only
  populated by `scripts/build.sh`. Build-only, not runtime.
- `verification/` — qualification fixtures
  (`matrices/*.yaml`, `scenarios/*.yaml`, `programs/*.yaml`,
  `deployments/*.yaml`). Driven by `scripts/run-scenarios.py` and
  `scripts/qualify-*.sh`. Qualification output goes to
  `${MANYFORGE_QUALIFICATION_OUTPUT_DIR:-/tmp/manyforge-qualification}`
  and sibling temp dirs, not into `verification/`.

## Observability REST endpoints

All paths are HTTP routes on the Composer backend (default
`http://127.0.0.1:9000`). The full route list with parameters lives in
[`../reference/REST_API.md`](../reference/REST_API.md). Below is the
operator-facing subset grouped by what you'd query for.

### Health / readiness

| Endpoint                         | Use it when                                                                                                                 |
| -------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| `GET /api/health`                | First check after starting — "is Composer alive?". Returns `"ok"` plus a per-component map (backend/catalog/program/cycle). |
| `GET /api/infra/status`          | Full infrastructure picture for all components.                                                                             |
| `GET /api/infra/heartbeat`       | Compact heartbeat payload — what the launcher's `status` verb polls.                                                        |
| `GET /api/infra/model-endpoints` | Configured model endpoints + best-effort live `/v1/models` discovery. Use to confirm the vLLM endpoint is reachable.        |
| `GET /api/runtime/status`        | Merged Composer + kernel + external-runtime status. Use this for the "is everything green?" check.                          |

### Execution state

| Endpoint                                                | Use it when                                                                                                                                                                                                             |
| ------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET /api/cycle/status`                                 | Cycle state (`idle`/`running`/`stopped`/`direct_control`) + velocity scale.                                                                                                                                             |
| `POST /api/cycle/recovery-actions`                      | Apply one exact action from the versioned `recoveryOffer` returned by cycle status/digest. Do not replay a cached offer after a conflict.                                                                               |
| `GET /api/kernel/snapshot`                              | Current kernel execution snapshot, including `state` and structured `error` if any. **First stop when motion fails.**                                                                                                   |
| `GET /api/kernel/metrics`                               | Kernel performance counters.                                                                                                                                                                                            |
| `GET /api/kernel/events?since=N`                        | Real-time kernel event stream — state transitions, plan/execute lifecycle, cache hits, collisions, interrupts. Poll with the last `event_index` as `since`.                                                             |
| `GET /api/execution/digest`                             | High-level execution digest (programatic equivalent of what the UI shows).                                                                                                                                              |
| `GET /api/execution/history?...`                        | Filtered event log. Filters: `eventTypes`, `minSeverity`, `components`, `startNs`/`endNs`, `executionId`, `programRunId`, `interventionRunId`, `run_id`. **Use this to extract the failure cause from a finished run.** |
| `GET /api/execution/alarms`                             | Current four-state alarms, attention/standing counts, reconciliation health, and bounded lifecycle history. Raw event severity does not imply a current alarm.                                                          |
| `POST /api/execution/alarms/{condition_id}/acknowledge` | Acknowledge one exact activation/revision. This records awareness only; it does not clear the condition or recover motion.                                                                                              |
| `POST /api/execution/alarms/acknowledge-all`            | Confirm and acknowledge the exact returned membership/generation. Intended for multiple attention alarms; stale membership is rejected.                                                                                 |
| `GET /api/execution/blackboard`                         | JSON-safe blackboard snapshot.                                                                                                                                                                                          |

The four alarm states are `active_unacknowledged`, `active_acknowledged`,
`returned_unacknowledged`, and `returned_acknowledged`. An acknowledged active
alarm is still a standing abnormal condition. Only its catalogued source can
report return-to-normal. Recovery acknowledgement (`acknowledge_and_continue`)
is a separate, non-moving action in the current cycle recovery offer.

### Scene + visualization

| Endpoint                                      | Use it when                                                                                                                                                                                                                                       |
| --------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET /api/scene/diagnostics`                  | Compare backend-authoritative scene truth with native viewer render state. Use when the viewer looks wrong.                                                                                                                                       |
| `GET /api/scene/viewer/status`                | Inspect viewer demand, bounded fanout/worker state, renderer identity, software-renderer detection, effective DPR, and requested frame-data rate. A `renderer_software` client is a diagnostic fallback, not a hardware-GPU qualification result. |
| `GET /api/scene/snapshot?width&height&format` | Render the active 3D viewport as an image. Returns 503 if no native viewer source is connected.                                                                                                                                                   |
| `GET /api/scene/snapshot/status`              | Inspect bounded snapshot-capture admission, pending request, rejection, timeout, and upload counters.                                                                                                                                             |
| `GET /api/scene/snapshots/{snapshot_id}`      | Serve a previously captured snapshot by stable id.                                                                                                                                                                                                |

During program/deployment cutover, inspect `sceneInitialization` in program/load
responses and the scene-initialization block in `GET /api/infra/heartbeat`.
`waiting` means the generation-fenced reset/scene seed has not completed; it is
not itself a pose-apply failure. A definitive failure is separately alarmed.

The in-view **Tools > Viewer rate** selector controls only the current browser's
bounded NRT frame request (`Auto`, 2, 5, 10, 15, or 30 Hz). `Auto` requests
15 Hz for both hardware and software renderers. Select a lower explicit rate
when software rendering or same-host load needs a tighter bound. External object
freshness is controlled separately by the applied deployment pose source's
`max_update_hz`; increasing the viewer rate does not increase that source's
acquisition rate.

### Diagnostic capture

| Endpoint                                     | Use it when                                                                                                                                                                 |
| -------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET /api/debug/scene-trace?limit=200`       | Current backend/native-viewer scene trace data.                                                                                                                             |
| `GET /api/debug/export?run_id=…`             | **Single-call debug bundle** for a run — capture this for bug reports.                                                                                                      |
| `GET /api/debug/export/ros2-medkit?run_id=…` | Normalized trace export for ros2_medkit-style tooling (schema pinned in specs `docs/spec/520` §7.2.1; full medkit integration runbook: [`ROS2_MEDKIT.md`](ROS2_MEDKIT.md)). |
| `POST /api/debug/signals/publish`            | Manually publish a signal sample into the runtime signal registry — for interrupt-source probing.                                                                           |

## Diagnostic scripts

These are operator-runnable scripts under `scripts/` that capture
observability artifacts.

### Status verbs

- **`./scripts/launch.sh status`** — print component state + popup URL
  (polls `/api/infra/heartbeat` + `/api/runtime/status`). Default
  diagnostic command after a `start`.
- **`./scripts/launch.sh preflight`** — port/docker/image preflight
  only. Use when `start` fails immediately.
- **`./scripts/launch.sh smoke`** — full bring-up + smoke probe;
  writes `$LOG_DIR/launcher-smoke.json`.
- **Removed `scripts/manyforge-stack.sh status` baseline** — the former
  `docker ps`/`api/health` diagnostic was exclusive to the deleted legacy
  mux/plugin transport. Its source is retained in Git history at
  `manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`; it is not a current
  command or restoration target.

### Smoke tests

| Script                                       | Purpose                                                                                                                                          |
| -------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| `scripts/smoke-assistant-e2e.py`             | Live ManyForge assistant E2E smoke + sampling-profile probe. Expects a live vLLM endpoint + live Composer.                                       |
| `scripts/smoke-assistant-provider-bridge.py` | Smoke-test a ManyForge assistant provider endpoint (v0.1 envelope, fail-closed checks).                                                          |
| `scripts/smoke-assistant-scene-authoring.py` | Smoke the deterministic scene-authoring path (`inspect_isaac_scene → propose_scene_objects`).                                                    |
| `scripts/manyforge-openclaw-smoke.sh`        | Direct smoke plus legacy mux/plugin diagnostics. Modes: `direct`, `mux`, `all`; `mux`/`all` require `MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true`. |

### Qualification harnesses

| Script                                                    | Purpose                                                                                                                                  | Output dir env var                                                            |
| --------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- |
| `scripts/qualify-scenarios.sh`                            | Core ManyForge qualification matrix in a production-like local process                                                                   | `MANYFORGE_QUALIFICATION_OUTPUT_DIR` (default `/tmp/manyforge-qualification`) |
| `scripts/qualify-native-scenarios.sh`                     | Native-live evaluation matrix with auto kernel mode                                                                                      | `MANYFORGE_NATIVE_QUALIFICATION_OUTPUT_DIR`                                   |
| `scripts/qualify-soak-scenarios.sh`                       | Long-duration soak qualification                                                                                                         | `MANYFORGE_SOAK_QUALIFICATION_OUTPUT_DIR`                                     |
| `scripts/qualify-visual-state-live.py`                    | Read-only probe of Composer visual-state snapshot routing during a live cycle                                                            | (stdout)                                                                      |
| `scripts/run-collision-engine-qualification.sh`           | Neutral `manyforge_collision` engine fixture/parity/latency probe                                                                        | (writes Phase-4 JSON)                                                         |
| `scripts/run-collision-mapper-perf.sh`                    | Docker-only compiler-emitted symbol/body lookup performance tripwire using a prebuilt retained-product consumer                          | Compact result on stdout                                                      |
| `scripts/qualification/run_i6_n7_product_matrix.py`       | Replays the eight maintained I6 native outcomes from one prebuilt retained package and executable set; raw process logs remain temporary | Compact JSON on stdout                                                        |
| `scripts/run-kernel-architecture-phase4-qualification.sh` | Kernel Architecture Sprint Phase 4 release-qualification harness (strict by default)                                                     | `MANYFORGE_PHASE4_OUT_DIR`                                                    |
| `scripts/verify-kernel-architecture-phase4.py`            | Stricter verifier over Phase 4 evidence; `--allow-incomplete` for local                                                                  | (stdout)                                                                      |

## Environment variables that affect observability

| Env var                                                                                                      | What it controls                                                                                                                                                                                                                                          |
| ------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `LOG_LEVEL`                                                                                                  | Historical assistant-supervisor log level (default INFO), consumed by captured blob `8c0ecee5ad79b553063c0f171740a3ccf617e8d7`; the current pure reducer does not consume it.                                                                             |
| `LOG_DIR`                                                                                                    | Launcher log root. Default `/tmp/manyforge-assistant-e2e`. Set in `scripts/lib/assistant.sh:56`.                                                                                                                                                          |
| `BRIDGE_AUDIT_LOG`                                                                                           | Path to the assistant audit JSONL. Default `$XDG_STATE_HOME/manyforge/assistant-bridge-audit.log`, size-rotated via `BRIDGE_AUDIT_LOG_MAX_BYTES`/`BRIDGE_AUDIT_LOG_KEEP`; launcher pins to `$LOG_DIR/known-good-bridge-audit.jsonl`.                      |
| `BRIDGE_LOG`, `SUPERVISOR_LOG`                                                                               | Per-process stdout/stderr capture paths inside `$LOG_DIR`.                                                                                                                                                                                                |
| `MANYFORGE_PROGRESS_MODE=plain`                                                                              | Force plain (no ANSI redraw) launcher progress output for CI logs.                                                                                                                                                                                        |
| `MANYFORGE_VIEWER_MAX_FRAME_HZ`                                                                              | Server ceiling for demand-driven viewer transform publication.                                                                                                                                                                                            |
| `MANYFORGE_VIEWER_DEBUG_SNAPSHOT_STALE_AFTER_MS`                                                             | Staleness threshold for the retained low-rate/debug scene snapshot projection.                                                                                                                                                                            |
| `MANYFORGE_VALIDATION_COLLISION_WORKERS`                                                                     | Direct-factory development/CI fallback for trajectory-validation worker count. Production uses the materialized `orchestrator.config.validation_collision_workers` deployment property (default 4); this variable never overrides an explicit deployment. |
| `MANYFORGE_CORS_ORIGINS`                                                                                     | CORS allowlist.                                                                                                                                                                                                                                           |
| `MANYFORGE_FS_ROOTS`                                                                                         | Filesystem-read allowlist for scene/mesh routes.                                                                                                                                                                                                          |
| `MANYFORGE_ASSISTANT_ENDPOINT_URL` / `MANYFORGE_NEMOCLAW_ASSISTANT_URL` / `MANYFORGE_OPENCLAW_ASSISTANT_URL` | Runtime endpoint URL the assistant provider dials. **Required for live (non-mock) chat.**                                                                                                                                                                 |
| `MANYFORGE_ASSISTANT_TIMEOUT_S`                                                                              | Per-request HTTP timeout for the assistant provider.                                                                                                                                                                                                      |
| `MANYFORGE_ASSISTANT_API_TOKEN`                                                                              | Optional bearer for the upstream bridge.                                                                                                                                                                                                                  |
| `MANYFORGE_ASSISTANT_PROVIDER`                                                                               | Selects `openclaw` vs `nemoclaw` provider lane (compose-side; default in `docker-compose.yaml:117`).                                                                                                                                                      |
| `MANYFORGE_RUN_SOAK_TESTS=1`                                                                                 | Gate to enable soak/load qualification pytest.                                                                                                                                                                                                            |
| `COMPOSER_READY_TIMEOUT_S`, `VLLM_MODEL_READY_TIMEOUT_S`, `ROS_DISCOVERY_TIMEOUT_S`                          | Launcher timeouts for `start` external-readiness probes — see [`LAUNCHER_QUICKSTART.md`](LAUNCHER_QUICKSTART.md).                                                                                                                                         |

## Container names

After `./scripts/launch.sh start`, expect these containers (visible
via `docker ps`):

- **manyforge-only lane.** A single launcher-spawned container,
  `manyforge-e2e-composer` (override with `COMPOSER_CONTAINER`).
  Created/cleaned by `scripts/lib/compose.sh`. If assistant is on, a
  second container `manyforge-e2e-vllm` runs the vLLM model server.
- **isaac-sim / ros-full lane.** Docker Compose services
  `composer-ros` and `isaac-topic-control` (project-prefixed as
  `<project>-composer-ros-1`, `<project>-isaac-topic-control-1`).
  Both run with `network_mode: host` and `ipc: host`
  (`docker-compose.yaml:104-105,135-136`) to share DDS discovery with
  Isaac.
- **Historical NemoClaw mux/plugin path.** The removed
  `scripts/manyforge-stack.sh` used `${CONTAINER_PREFIX:-manyforge-dev}-…`
  naming and an MCP-enabled `manyforge-dev-mcp` variant. These names are
  archaeology only, not current process-discovery inputs.

To tail Composer logs in the right lane:

```bash
# manyforge-only lane
docker logs -f manyforge-e2e-composer

# isaac-sim / ros-full lane
docker compose logs -f --tail 100 composer-ros

# vLLM (assistant on)
docker logs -f manyforge-e2e-vllm
```

## Capturing a bug report

For a reproducible failure, capture the following before tearing down:

1. Grab `GET /api/debug/export?run_id=<id>` — the single-call debug
   bundle.
1. Save `GET /api/execution/history?run_id=<id>&limit=1000` for the
   full filtered event log.
1. Save `GET /api/kernel/snapshot` for the active kernel state +
   structured error.
1. Save `GET /api/execution/alarms` and `GET /api/cycle/status` so the
   alarm lifecycle and exact recovery offer are preserved.
1. `docker logs --tail 2000 manyforge-e2e-composer > composer.log`
   (or the equivalent service name for the lane).
1. If assistant: copy `$LOG_DIR/known-good-bridge-audit.jsonl` and
   `$LOG_DIR/known-good-bridge.log`.
1. Note the launcher invocation (lane + assistant + scenario), the deployment's
   requested/reported-effective collision-validation worker counts, and the
   value of `MANYFORGE_VALIDATION_COLLISION_WORKERS` only when a direct-factory
   development/CI run used that fallback.
