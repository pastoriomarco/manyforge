<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py headless-rest
  Last regenerated: 2026-08-09 06:34:49 UTC
  Generator source: scripts/regen-public-contracts.py:gen_headless_rest_api
-->

# Headless Runtime REST API

This is the independent headless projection over Manager, logical-scene, scene-runtime, Alarm Lifecycle, Historian, and the selected addressed execution owner. Reads return owner-issued facts. Alarm acknowledgement, Manager Apply, execution submission, and no-motion preview are attributed intents; the API never owns or reduces execution state. Preview cannot dispatch, and approval uses the ordinary operation endpoint. The bounded I6 execution path does not by itself make Manager Apply's separate Kernel fence operational. No condition mutation endpoint exists. `/api/health` reports only `headless_projection_ready`; robot readiness has no H0 owner and is explicitly unavailable. The alarm page defaults to 32 records and is bounded at 128, history defaults to and is bounded at 32 records, request bodies are bounded at 1 MiB, and native/HTTP responses are bounded at 64 MiB.

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Headless Runtime API`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `GET` | `/api/assistant/modes/{mode_id}` |  | Assistant Mode |
| `GET` | `/api/execution/alarms` |  | Alarms |
| `POST` | `/api/execution/alarms/acknowledge-all` |  | Acknowledge All |
| `GET` | `/api/execution/alarms/metrics` |  | Alarm Metrics |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` |  | Acknowledge Alarm |
| `GET` | `/api/execution/history` |  | History |
| `GET` | `/api/execution/history/health` |  | Historian Health |
| `POST` | `/api/execution/history/reconcile` |  | Historian Reconcile |
| `POST` | `/api/execution/motion-previews` |  | Execution Preview Motion |
| `POST` | `/api/execution/operations` |  | Execution Submit |
| `GET` | `/api/execution/operations/{operation_id}` |  | Execution Read |
| `POST` | `/api/execution/operations/{operation_id}/acknowledge-completed` |  | Execution Acknowledge Completed |
| `POST` | `/api/execution/operations/{operation_id}/pause` |  | Execution Pause |
| `POST` | `/api/execution/operations/{operation_id}/reset` |  | Execution Reset |
| `POST` | `/api/execution/operations/{operation_id}/resume` |  | Execution Resume |
| `GET` | `/api/execution/operations/{operation_id}/runtime-scale` |  | Execution Get Runtime Scale |
| `POST` | `/api/execution/operations/{operation_id}/runtime-scale` |  | Execution Set Runtime Scale |
| `POST` | `/api/execution/pick-place/plans/pick` |  | Execution Plan Pick |
| `POST` | `/api/execution/pick-place/plans/place` |  | Execution Plan Place |
| `POST` | `/api/execution/pick-place/sequences/pick` |  | Execution Start Pick |
| `POST` | `/api/execution/pick-place/sequences/place` |  | Execution Start Place |
| `GET` | `/api/execution/pick-place/sequences/{sequence_id}` |  | Execution Pick Place Sequence |
| `GET` | `/api/execution/status` |  | Execution Status |
| `POST` | `/api/execution/stops` |  | Execution Stop |
| `GET` | `/api/health` |  | Health |
| `POST` | `/api/manager/apply` |  | Manager Apply |
| `GET` | `/api/manager/diff` |  | Manager Diff |
| `GET` | `/api/manager/graph` |  | Manager Graph |
| `GET` | `/api/manager/status` |  | Manager Status |
| `GET` | `/api/node-environment` |  | Node Environment |
| `GET` | `/api/runtime` |  | Runtime |
| `GET` | `/api/scene` |  | Scene |
| `POST` | `/api/scene/transactions` |  | Scene Commit |
| `GET` | `/api/source-model` |  | Source Model |
