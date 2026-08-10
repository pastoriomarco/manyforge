<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py headless-rest
  Last regenerated: 2026-08-10 09:36:36 UTC
  Generator source: scripts/regen-public-contracts.py:gen_headless_rest_api
-->

# Headless Runtime REST API

This is the independent headless projection over Manager, logical-scene, scene-runtime, Alarm Lifecycle, Historian, and the selected addressed execution owner and independent Program runner. Reads return owner-issued facts. Alarm acknowledgement, Manager and Program Apply, execution submission, no-motion preview, Program Run, and explicit Program Stop are attributed intents; the API never owns or reduces execution or Program-run state. Preview cannot dispatch, and approval uses the ordinary operation endpoint. An unconfigured or lost Program runner is reported as unavailable and never interpreted as idle. No condition mutation endpoint exists. `/api/health` reports only `headless_projection_ready`; robot readiness has no H0 owner and is explicitly unavailable. The alarm page defaults to 32 records and is bounded at 128, history defaults to and is bounded at 32 records, ordinary request bodies are bounded at 1 MiB, Program Apply at 17 MiB, model-attempt audit at 256 KiB, native/HTTP responses at 64 MiB, and private Program-runner responses at 32 MiB.

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Headless Runtime API`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `GET` | `/api/assistant/modes` |  | Assistant Modes |
| `GET` | `/api/assistant/modes/{mode_id}` |  | Assistant Mode |
| `GET` | `/api/execution/alarms` |  | Alarms |
| `POST` | `/api/execution/alarms/acknowledge-all` |  | Acknowledge All |
| `GET` | `/api/execution/alarms/metrics` |  | Alarm Metrics |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` |  | Acknowledge Alarm |
| `POST` | `/api/execution/capability-command-previews` |  | Execution Preview Capability Command |
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
| `GET` | `/api/program/applied` |  | Program Applied |
| `POST` | `/api/program/apply` |  | Program Apply |
| `POST` | `/api/program/model-attempts` |  | Program Model Attempt |
| `GET` | `/api/program/runs` |  | Program Runs |
| `POST` | `/api/program/runs` |  | Program Run Start |
| `GET` | `/api/program/runs/{program_run_id}` |  | Program Run Read |
| `POST` | `/api/program/runs/{program_run_id}/stop` |  | Program Run Stop |
| `GET` | `/api/runtime` |  | Runtime |
| `GET` | `/api/scenarios` |  | Scenarios |
| `GET` | `/api/scenarios/{scenario_id}/program-selection` |  | Scenario Program Selection |
| `GET` | `/api/scene` |  | Scene |
| `POST` | `/api/scene/resource-poses/query` |  | Scene Resource Poses |
| `POST` | `/api/scene/transactions` |  | Scene Commit |
| `GET` | `/api/source-model` |  | Source Model |
| `GET` | `/api/viewer-model/resource` |  | Viewer Model Resource |
| `GET` | `/api/viewer-model/urdf` |  | Viewer Model Urdf |
