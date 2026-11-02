<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py headless-rest
  Last regenerated: 2026-11-02 14:57:37 UTC
  Generator source: scripts/regen-public-contracts.py:gen_headless_rest_api
-->

# Headless Runtime REST API

This is the independent headless projection over Manager, logical-scene, scene-runtime, Alarm Lifecycle, Historian, and the selected addressed execution owner and independent Program runner. Reads return owner-issued facts. Alarm acknowledgement, Manager and Program Apply, execution submission, no-motion preview, Program Run, explicit Program Stop, and component actions are attributed intents. Generic `/api/control/state` and `/api/control/commands` provide the freshness-aware REST form of the same transport-neutral control ingress used by `/ws/control`; no command is automatically replayed. The API never owns or reduces execution, Program-run, lifecycle, or controller-route state. Preview cannot dispatch, and approval uses the ordinary operation endpoint. An unconfigured or lost Program runner makes Program-owned routes unavailable and is never interpreted as idle; independently owned native operation and Stop routes remain available when their owner is configured. No condition mutation endpoint exists. `/api/health` reports only `headless_projection_ready`; robot readiness has no H0 owner and is explicitly unavailable. The alarm page defaults to 32 records and is bounded at 128, history defaults to and is bounded at 32 records, ordinary request bodies are bounded at 1 MiB, Program Apply at 17 MiB, model-attempt audit at 256 KiB, native/HTTP responses at 64 MiB, and private Program-runner responses at 32 MiB. Program diagnostic captures are separately limited to 64 KiB and asynchronously written by the existing Historian. Their producer-clock values, omissions and delivery losses are available through History/debug export and recorder health, not the high-rate control/viewer stream. Skill expansion is effect-free, selects one exact installed NodeEnvironment and requires ordinary Program/Binding validation before any subsequent execution.

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Headless Runtime API`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `POST` | `/api/activation-preparations` |  | Prepare Activation |
| `GET` | `/api/assistant/modes` |  | Assistant Modes |
| `GET` | `/api/assistant/modes/{mode_id}` |  | Assistant Mode |
| `GET` | `/api/assistant/modes/{mode_id}/context` |  | Assistant Mode Context |
| `GET` | `/api/cell/authoring-admission` |  | Authoring Admission |
| `GET` | `/api/cell/latch` |  | Cell Latch |
| `GET` | `/api/components` |  | Components |
| `POST` | `/api/components/actions` |  | Component Group Action |
| `POST` | `/api/components/{component_id}/actions` |  | Component Action |
| `POST` | `/api/control/commands` |  | Execution Control Command |
| `GET` | `/api/control/diagnostics` |  | Execution Control Diagnostics |
| `GET` | `/api/control/state` |  | Execution Control Projection |
| `POST` | `/api/cycle/recovery-actions` |  | Cycle Recovery Action |
| `GET` | `/api/cycle/status` |  | Program Cycle Status |
| `GET` | `/api/debug/export` |  | Debug Export |
| `GET` | `/api/debug/export/ros2-medkit` |  | Normalized Trace Export |
| `GET` | `/api/execution/alarms` |  | Alarms |
| `POST` | `/api/execution/alarms/acknowledge-all` |  | Acknowledge All |
| `GET` | `/api/execution/alarms/metrics` |  | Alarm Metrics |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` |  | Acknowledge Alarm |
| `POST` | `/api/execution/capability-command-previews` |  | Execution Preview Capability Command |
| `GET` | `/api/execution/control-state` |  | Execution Control State |
| `GET` | `/api/execution/diagnostics` |  | Execution Diagnostics |
| `POST` | `/api/execution/full-stop` |  | Execution Full Stop |
| `GET` | `/api/execution/history` |  | History |
| `POST` | `/api/execution/history/apply-outcomes` |  | History Apply Outcome |
| `GET` | `/api/execution/history/captures` |  | History Captures |
| `GET` | `/api/execution/history/discovery` |  | History Discovery |
| `GET` | `/api/execution/history/export` |  | History Export |
| `GET` | `/api/execution/history/health` |  | Historian Health |
| `POST` | `/api/execution/history/reconcile` |  | Historian Reconcile |
| `GET` | `/api/execution/history/{sequence}` |  | History Detail |
| `GET` | `/api/execution/identity` |  | Execution Identity |
| `POST` | `/api/execution/io-signals/mock-input` |  | Execution Set Mock Io Input |
| `POST` | `/api/execution/io-signals/read` |  | Execution Read Io Signal |
| `POST` | `/api/execution/manual-handover` |  | Execution Read Manual Handover |
| `POST` | `/api/execution/motion-previews` |  | Execution Preview Motion |
| `POST` | `/api/execution/operations` |  | Execution Submit |
| `GET` | `/api/execution/operations/{operation_id}` |  | Execution Read |
| `POST` | `/api/execution/operations/{operation_id}/acknowledge-completed` |  | Execution Acknowledge Completed |
| `POST` | `/api/execution/operations/{operation_id}/reset` |  | Execution Reset |
| `POST` | `/api/execution/pick-place/plans/pick` |  | Execution Plan Pick |
| `POST` | `/api/execution/pick-place/plans/place` |  | Execution Plan Place |
| `GET` | `/api/execution/program-runs/{program_run_id}/admitted-speeds` |  | Program Run Admitted Speeds |
| `GET` | `/api/execution/runtime-scale-control` |  | Execution Get Runtime Scale Control |
| `POST` | `/api/execution/runtime-scale-control` |  | Execution Set Runtime Scale Control |
| `POST` | `/api/execution/stops` |  | Execution Stop |
| `GET` | `/api/execution/targets` |  | Execution Targets |
| `GET` | `/api/health` |  | Health |
| `GET` | `/api/io` |  | Io Signals |
| `POST` | `/api/io/{signal_id}/set` |  | Set Io Output |
| `POST` | `/api/manager/apply` |  | Manager Apply |
| `POST` | `/api/manager/apply/acknowledge` |  | Manager Apply Acknowledge |
| `POST` | `/api/manager/apply/release-for-new` |  | Manager Apply Release For New |
| `GET` | `/api/manager/apply/status` |  | Manager Apply Status |
| `POST` | `/api/manager/deployment-candidates/preview` |  | Manager Deployment Candidate Preview |
| `GET` | `/api/manager/diff` |  | Manager Diff |
| `GET` | `/api/manager/graph` |  | Manager Graph |
| `GET` | `/api/manager/status` |  | Manager Status |
| `GET` | `/api/node-environment` |  | Node Environment |
| `GET` | `/api/program/applied` |  | Program Applied |
| `GET` | `/api/program/applied/status` |  | Program Applied Status |
| `POST` | `/api/program/authoring-transactions` |  | Acquire Authoring Transaction |
| `POST` | `/api/program/authoring-transactions/reconcile-owner` |  | Reconcile Authoring Owner |
| `POST` | `/api/program/authoring-transactions/recovery-receipt` |  | Recover Authoring Transaction |
| `POST` | `/api/program/authoring-transactions/settle` |  | Settle Authoring Transaction |
| `GET` | `/api/program/committed-activation` |  | Program Committed Activation |
| `POST` | `/api/program/cycle/reset` |  | Program Cycle Reset |
| `POST` | `/api/program/diagnostics` |  | Program Diagnostic |
| `GET` | `/api/program/diagnostics/health` |  | Program Diagnostic Health |
| `POST` | `/api/program/model-attempts` |  | Program Model Attempt |
| `GET` | `/api/program/persistent-values` |  | Program Persistent Values |
| `POST` | `/api/program/persistent-values` |  | Set Program Persistent Value |
| `POST` | `/api/program/persistent-values/clear` |  | Clear Program Persistent Values |
| `POST` | `/api/program/persistent-values/reset` |  | Reset Program Persistent Value |
| `GET` | `/api/program/readiness` |  | Program Readiness |
| `GET` | `/api/program/runs` |  | Program Runs |
| `POST` | `/api/program/runs` |  | Program Run Start |
| `GET` | `/api/program/runs/{program_run_id}` |  | Program Run Read |
| `POST` | `/api/program/runs/{program_run_id}/blackboard-values` |  | Write Runtime Blackboard |
| `POST` | `/api/program/runs/{program_run_id}/full-stop` |  | Program Run Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/manual-handover` |  | Program Run Manual Handover |
| `POST` | `/api/program/runs/{program_run_id}/manual-result` |  | Stage Manual Result |
| `POST` | `/api/program/runs/{program_run_id}/motion-start` |  | Program Run Motion Start |
| `POST` | `/api/program/runs/{program_run_id}/motion-stop` |  | Program Run Motion Stop |
| `POST` | `/api/program/runs/{program_run_id}/node-correction-previews` |  | Program Node Correction Preview |
| `POST` | `/api/program/runs/{program_run_id}/reset` |  | Program Run Reset Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/resume-full-stop` |  | Program Run Resume Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/resume-motion` |  | Program Run Motion Resume |
| `GET` | `/api/program/runs/{program_run_id}/runtime-controls` |  | Program Runtime Controls |
| `POST` | `/api/program/runs/{program_run_id}/stop` |  | Program Run Stop |
| `POST` | `/api/program/scene-preparations` |  | Program Scene Prepare |
| `GET` | `/api/runtime` |  | Runtime |
| `GET` | `/api/scene` |  | Scene |
| `POST` | `/api/scene/resource-poses/query` |  | Scene Resource Poses |
| `POST` | `/api/scene/transactions` |  | Scene Commit |
| `POST` | `/api/scene/transactions/status` |  | Scene Transaction Status |
| `GET` | `/api/skills` |  | Skills |
| `POST` | `/api/skills/expand` |  | Expand Skill |
| `GET` | `/api/source-model` |  | Source Model |
| `GET` | `/api/source-model-context` |  | Source Model Context |
| `GET` | `/api/viewer-model/resource` |  | Viewer Model Resource |
| `GET` | `/api/viewer-model/urdf` |  | Viewer Model Urdf |

## Route notes

### `GET /api/source-model`

The applied cell's source-local model facts, schema `manyforge.source_model_projection.v3`: `occurrences` (each model occurrence with its owning `component_id`, coordinates, links and SRDF facts), `targets` (each component command target with its frames, model groups and collision bodies) and `components`, one row per component of the applied Deployment, sorted by `component_id`. A row's `mounted_on` names the parent `{component_id, interface_id}` the component is mounted on, or is null. `source.source_content_digest` covers every field except `source`, so it changes with any of them. A reader that expects an older schema version refuses this one: reload an open Composer tab after an upgrade, and retry an assistant draft that was refused as stale. `GET /api/source-model-context` carries the same projection.
