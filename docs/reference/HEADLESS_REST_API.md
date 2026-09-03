<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py headless-rest
  Last regenerated: 2026-09-02 12:35:11 UTC
  Generator source: scripts/regen-public-contracts.py:gen_headless_rest_api
-->

# Headless Runtime REST API

This is the independent headless projection over Manager, logical-scene, scene-runtime, Alarm Lifecycle, Historian, and the selected addressed execution owner and independent Program runner. Reads return owner-issued facts. Alarm acknowledgement, Manager and Program Apply, execution submission, no-motion preview, Program Run, explicit Program Stop, and confirmed CCLC lifecycle/route requests are attributed intents; the API never owns or reduces execution, Program-run, lifecycle, or controller-route state. Preview cannot dispatch, and approval uses the ordinary operation endpoint. An unconfigured or lost Program runner is reported as unavailable and never interpreted as idle. No condition mutation endpoint exists. `/api/health` reports only `headless_projection_ready`; robot readiness has no H0 owner and is explicitly unavailable. The alarm page defaults to 32 records and is bounded at 128, history defaults to and is bounded at 32 records, ordinary request bodies are bounded at 1 MiB, Program Apply at 17 MiB, model-attempt audit at 256 KiB, native/HTTP responses at 64 MiB, and private Program-runner responses at 32 MiB.

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Headless Runtime API`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `GET` | `/api/assistant/modes` |  | Assistant Modes |
| `GET` | `/api/assistant/modes/{mode_id}` |  | Assistant Mode |
| `GET` | `/api/assistant/modes/{mode_id}/context` |  | Assistant Mode Context |
| `GET` | `/api/cclc/components/{component_id}` |  | Cclc Component Snapshot |
| `GET` | `/api/cclc/components/{component_id}/route-templates` |  | Cclc Route Templates |
| `POST` | `/api/cclc/lifecycle/requests` |  | Cclc Lifecycle Submit |
| `GET` | `/api/cclc/lifecycle/transactions/{transaction_id}` |  | Cclc Lifecycle Status |
| `GET` | `/api/cclc/owner` |  | Cclc Owner Identity |
| `POST` | `/api/cclc/routes/requests` |  | Cclc Route Submit |
| `GET` | `/api/cclc/routes/transactions/{transaction_id}` |  | Cclc Route Status |
| `GET` | `/api/cell/authoring-admission` |  | Authoring Admission |
| `GET` | `/api/component-overview` |  | Component Overview |
| `POST` | `/api/cycle/recovery-actions` |  | Cycle Recovery Action |
| `GET` | `/api/cycle/status` |  | Program Cycle Status |
| `GET` | `/api/execution/alarms` |  | Alarms |
| `POST` | `/api/execution/alarms/acknowledge-all` |  | Acknowledge All |
| `GET` | `/api/execution/alarms/metrics` |  | Alarm Metrics |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` |  | Acknowledge Alarm |
| `POST` | `/api/execution/capability-command-previews` |  | Execution Preview Capability Command |
| `GET` | `/api/execution/control-state` |  | Execution Control State |
| `GET` | `/api/execution/diagnostics` |  | Execution Diagnostics |
| `POST` | `/api/execution/full-stop` |  | Execution Full Stop |
| `GET` | `/api/execution/history` |  | History |
| `GET` | `/api/execution/history/health` |  | Historian Health |
| `POST` | `/api/execution/history/reconcile` |  | Historian Reconcile |
| `GET` | `/api/execution/history/{sequence}` |  | History Detail |
| `GET` | `/api/execution/identity` |  | Execution Identity |
| `POST` | `/api/execution/motion-previews` |  | Execution Preview Motion |
| `POST` | `/api/execution/operations` |  | Execution Submit |
| `GET` | `/api/execution/operations/{operation_id}` |  | Execution Read |
| `POST` | `/api/execution/operations/{operation_id}/acknowledge-completed` |  | Execution Acknowledge Completed |
| `POST` | `/api/execution/operations/{operation_id}/pause` |  | Execution Pause |
| `POST` | `/api/execution/operations/{operation_id}/reset` |  | Execution Reset |
| `POST` | `/api/execution/operations/{operation_id}/resume` |  | Execution Resume |
| `POST` | `/api/execution/pick-place/plans/pick` |  | Execution Plan Pick |
| `POST` | `/api/execution/pick-place/plans/place` |  | Execution Plan Place |
| `POST` | `/api/execution/pick-place/sequences/pick` |  | Execution Start Pick |
| `POST` | `/api/execution/pick-place/sequences/place` |  | Execution Start Place |
| `GET` | `/api/execution/pick-place/sequences/{sequence_id}` |  | Execution Pick Place Sequence |
| `GET` | `/api/execution/runtime-scale-control` |  | Execution Get Runtime Scale Control |
| `POST` | `/api/execution/runtime-scale-control` |  | Execution Set Runtime Scale Control |
| `GET` | `/api/execution/status` |  | Execution Status |
| `GET` | `/api/execution/stop-fault` |  | Execution Stop Fault |
| `POST` | `/api/execution/stop-fault/recover` |  | Execution Recover Stop Fault |
| `POST` | `/api/execution/stops` |  | Execution Stop |
| `GET` | `/api/execution/targets` |  | Execution Targets |
| `GET` | `/api/health` |  | Health |
| `POST` | `/api/manager/apply` |  | Manager Apply |
| `GET` | `/api/manager/apply/status` |  | Manager Apply Status |
| `POST` | `/api/manager/deployment-candidates/compile` |  | Compile Deployment Candidate |
| `POST` | `/api/manager/deployment-candidates/preview` |  | Manager Deployment Candidate Preview |
| `GET` | `/api/manager/diff` |  | Manager Diff |
| `GET` | `/api/manager/graph` |  | Manager Graph |
| `GET` | `/api/manager/status` |  | Manager Status |
| `GET` | `/api/node-environment` |  | Node Environment |
| `GET` | `/api/program/applied` |  | Program Applied |
| `POST` | `/api/program/apply` |  | Program Apply |
| `POST` | `/api/program/authoring-transactions` |  | Acquire Authoring Transaction |
| `POST` | `/api/program/authoring-transactions/reconcile-owner` |  | Reconcile Authoring Owner |
| `POST` | `/api/program/authoring-transactions/recovery-receipt` |  | Recover Authoring Transaction |
| `POST` | `/api/program/authoring-transactions/settle` |  | Settle Authoring Transaction |
| `POST` | `/api/program/cycle/reset` |  | Program Cycle Reset |
| `POST` | `/api/program/model-attempts` |  | Program Model Attempt |
| `GET` | `/api/program/runs` |  | Program Runs |
| `POST` | `/api/program/runs` |  | Program Run Start |
| `GET` | `/api/program/runs/{program_run_id}` |  | Program Run Read |
| `POST` | `/api/program/runs/{program_run_id}/blackboard-values` |  | Write Runtime Blackboard |
| `POST` | `/api/program/runs/{program_run_id}/full-stop` |  | Program Run Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/manual-result` |  | Stage Manual Result |
| `POST` | `/api/program/runs/{program_run_id}/motion-stop` |  | Program Run Motion Stop |
| `POST` | `/api/program/runs/{program_run_id}/reset` |  | Program Run Reset Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/resume-full-stop` |  | Program Run Resume Full Stop |
| `GET` | `/api/program/runs/{program_run_id}/runtime-controls` |  | Program Runtime Controls |
| `POST` | `/api/program/runs/{program_run_id}/runtime-overrides` |  | Stage Runtime Override |
| `POST` | `/api/program/runs/{program_run_id}/runtime-overrides/activate` |  | Activate Runtime Override |
| `POST` | `/api/program/runs/{program_run_id}/runtime-overrides/clear` |  | Clear Runtime Override |
| `POST` | `/api/program/runs/{program_run_id}/runtime-overrides/promote` |  | Promote Runtime Override |
| `POST` | `/api/program/runs/{program_run_id}/stop` |  | Program Run Stop |
| `POST` | `/api/program/scene-preparations` |  | Program Scene Prepare |
| `GET` | `/api/runtime` |  | Runtime |
| `GET` | `/api/scenarios` |  | Scenarios |
| `GET` | `/api/scenarios/{scenario_id}/load-selection` |  | Scenario Load Selection |
| `GET` | `/api/scenarios/{scenario_id}/program-selection` |  | Scenario Program Selection |
| `GET` | `/api/scene` |  | Scene |
| `POST` | `/api/scene/resource-poses/query` |  | Scene Resource Poses |
| `POST` | `/api/scene/transactions` |  | Scene Commit |
| `GET` | `/api/source-model` |  | Source Model |
| `GET` | `/api/source-model-context` |  | Source Model Context |
| `GET` | `/api/viewer-model/resource` |  | Viewer Model Resource |
| `GET` | `/api/viewer-model/urdf` |  | Viewer Model Urdf |
