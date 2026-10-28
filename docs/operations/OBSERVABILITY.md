# Observability — Logs, Endpoints, Diagnostic Capture

Where ManyForge's logs go, which REST endpoints report which state, and what
to capture for a bug report.

For symptom-to-cause mapping see
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md). For the full route reference,
see [`../reference/REST_API.md`](../reference/REST_API.md) (generated from the
FastAPI app).

## Logs

ManyForge does **not** ship a unified log file. All workbench processes write
to the workbench container's console, captured by Docker; runtime and alarm
events go to the Historian and are read through REST; the assistant bridge
also writes a JSONL audit file.

### Workbench console

- **Where**: the workbench container's console. Child-process output
  (Composer, Headless API, Program runner, ROS host) is inherited by it:
  `docker logs --tail 200 manyforge-development-workbench` (or the
  `MANYFORGE_DEV_CONTAINER_NAME` you set). Docker rotates it at 50 MB × 3
  files. `./scripts/compose-dev.sh stop` keeps the last 10 MB under the runtime
  root's `evidence/` (see [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md)).
- **Composer backend format**: single-line JSON per request (path, status,
  duration_ms, client) plus service-level INFO/WARNING/ERROR, from
  `manyforge_composer/backend/logging_config.py`. `uvicorn.access` is quieted
  to WARNING. The level is INFO by default (Composer's `--log-level` option).

### Assistant bridge audit

The workbench writes the assistant audit JSONL to
`/tmp/manyforge-state/current/assistant-bridge/audit.jsonl` inside the
container, which is `.manyforge-dev/runtime/<container-name>/current/assistant-bridge/audit.jsonl`
on the host (`MANYFORGE_ASSISTANT_AUDIT_LOG`). It is rotated at 10 MB, 3 files
kept. It records every assistant turn, fail-closed rejection and upstream
error. On the next start, `current/` moves to `previous-sessions/`.

### Runtime events

Runtime and alarm events are written asynchronously to the bounded Headless
Historian. Composer does not buffer, merge, or persist a second event stream.
The public read surface is:

- `GET /api/execution/history` — one bounded 32-row summary page;
- `GET /api/execution/history/{sequence}` — exact detail for one selected row;
- `GET /api/execution/history/health` — Historian storage health; and
- `GET /api/execution/alarms` — current condition-backed alarms.

The browser mounts only the current summary page and requests detail on
selection.

### What is **not** a runtime log

- `verification/` — qualification fixtures used by the
  `scripts/qualification/` entrypoints, not runtime output.

## Observability REST endpoints

All paths are HTTP routes on the Composer API, `http://127.0.0.1:9020` in the
workbench. The full route list with parameters lives in
[`../reference/REST_API.md`](../reference/REST_API.md). Below is the
operator-facing subset.

### Health / readiness

| Endpoint | Use it when |
| --- | --- |
| `GET /api/health` | First check after starting: is Composer alive? |
| `GET /api/runtime` | Runtime owner availability as seen by Composer. |
| `GET /api/component-overview` | Per-component readiness and status, as in Composer's component view. |
| `GET /api/scenarios/current` | The active Scenario and its identity. |

`./scripts/compose-dev.sh status` combines the owner-issued current Scenario
with `ready` and `unavailable_reasons`; it is the default check after `start`.

### Execution state

| Endpoint | Use it when |
| --- | --- |
| `GET /api/cycle/status` | Cycle state and the one current versioned `recovery_offer`. |
| `POST /api/cycle/recovery-actions` | Apply one exact action from that offer. Do not replay a cached offer after a conflict. |
| `GET /api/execution/history?...` | Filtered event page. Filters include `event_types`, `minimum_severity`, `components`, `program_run_id`, `operation_id`, `recent_seconds`, `search`; `limit` is at most 32, page with `before_sequence`. `search` is a case-insensitive substring (at least 3 characters) of the component, the event type, the message and reason codes, a component incident's cause, closure and vendor codes, the alarm subject, and the event's run/operation/Stop/incident/condition ids; other payload fields are not searched. |
| `GET /api/execution/alarms` | Current four-state alarms. Raw event severity does not imply a current alarm. |
| `POST /api/execution/alarms/{condition_id}/acknowledge` | Acknowledge one exact activation/revision. This records awareness only; it does not clear the condition or recover motion. |
| `POST /api/execution/alarms/acknowledge-all` | Acknowledge the exact returned membership/generation; stale membership is rejected. |
| `GET /api/execution/stop-fault` | Current Stop fault, if any. |

The four alarm states are `active_unacknowledged`, `active_acknowledged`,
`returned_unacknowledged`, and `returned_acknowledged`. An acknowledged active
alarm is still a standing abnormal condition. Only its catalogued source can
report return-to-normal. Recovery acknowledgement (`acknowledge_and_continue`)
is a separate, non-moving action in the current cycle recovery offer.

### Scene + visualization

| Endpoint | Use it when |
| --- | --- |
| `GET /api/scene` | The backend-authoritative scene. Use when the viewer looks wrong. |
| `GET /api/scene/viewer/status` | Viewer stream demand, client and renderer state. |
| `GET /api/scene/snapshot/render?width=&height=&format=` | Render the current 3D view as `jpeg` or `png`. Needs an open Composer viewer. |
| `GET /api/scene/snapshot/status` | Snapshot-capture counters and last rejection/timeout. |

The viewer settings (gear at the top right) set the robot update rate shown
in the browser. This changes only the display, never the robot's motion or a
pose source's acquisition rate.

### Diagnostic capture

| Endpoint | Use it when |
| --- | --- |
| `GET /api/debug/export?run_id=…` | **Single-call debug bundle** for a Program run; capture this for bug reports. |
| `GET /api/debug/export/ros2-medkit?run_id=…` | Normalized trace of a run in a ManyForge-native format (`manyforge.ros2_medkit_trace.v2`); no ros2_medkit tool reads it. |

## Status commands

- **`./scripts/compose-dev.sh status`** — the owner-issued current Scenario
  with `ready` and `unavailable_reasons`.
- **`docker inspect --format '{{.State.Status}} {{.State.ExitCode}}' manyforge-development-workbench`**
  — container state and exit code.
- **`./scripts/compose-dev.sh stop`** — stops the workbench and keeps a
  bounded console tail under the runtime root's `evidence/`.

## Environment variables that affect observability

| Env var | What it controls |
| --- | --- |
| `MANYFORGE_DEV_CONTAINER_NAME` | Workbench container name (default `manyforge-development-workbench`). |
| `MANYFORGE_MOTION_TRACE_DIR` | Optional motion-trace snapshots for debug export (see [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md#reference)). |
| `MANYFORGE_LOG_EVIDENCE_MAX_DISK_PERCENT`, `MANYFORGE_LOG_EVIDENCE_MIN_FREE_BYTES` | Log/evidence retention triggers (see the workbench guide). |

## Container names

The workbench runs as one container, `manyforge-development-workbench`
(override with `MANYFORGE_DEV_CONTAINER_NAME`), visible via `docker ps`. It
uses host networking to share DDS discovery with Isaac or the robot driver.

```bash
docker logs -f --tail 100 manyforge-development-workbench
```

## Capturing a bug report

For a reproducible failure, capture the following before stopping
(`API=http://127.0.0.1:9020`):

1. `GET $API/api/debug/export?run_id=<program_run_id>` — the single-call debug
   bundle.
1. `GET $API/api/execution/history?program_run_id=<program_run_id>` for the
   run's latest events.
1. `GET $API/api/execution/alarms` and `GET $API/api/cycle/status` so the
   alarm lifecycle and exact recovery offer are preserved.
1. `docker logs --tail 2000 manyforge-development-workbench > workbench.log`
   and the `./scripts/compose-dev.sh status` output.
1. If the assistant was used: copy its audit JSONL (see
   [§Assistant bridge audit](#assistant-bridge-audit)).
1. Note the start command (`./scripts/compose-dev.sh start --project …`).
