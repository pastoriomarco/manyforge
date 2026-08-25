<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py structural-rest
  Last regenerated: 2026-08-24 00:41:29 UTC
  Generator source: scripts/regen-public-contracts.py:gen_rest_api
-->

# Composer REST API

This is the structural, authoring, and thin runtime-client Composer surface retained during the composable-cell runtime cutover. Catalog reads, sandboxed artifact browsing, structural URDF assets, health, and the strict revision-fenced Program Draft/Saved workflow remain available. When Headless and the independent Program runner are configured, Composer also forwards scenario, Applied, Program Run, status, explicit Program Stop, and confirmed CCLC lifecycle/route requests without owning their state. Complete product launch and remaining scene/backend capabilities remain unavailable. The catch-all `/api/{unavailable_path}` operations return the typed runtime-unavailable response for former product routes.

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Composer (replaceable client)`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `POST` | `/api/assistant/bridge/tools/{tool_id}` | assistant-draft-callback | Invoke Draft Tool |
| `GET` | `/api/assistant/modes` | headless-runtime-client | Assistant Modes |
| `GET` | `/api/assistant/modes/{mode_id}` | headless-runtime-client | Assistant Mode |
| `GET` | `/api/catalog` | catalog | Get Catalog |
| `GET` | `/api/catalog/{entry_id}` | catalog | Get Catalog Entry |
| `GET` | `/api/cclc/components/{component_id}` | headless-runtime-client | Cclc Component |
| `GET` | `/api/cclc/components/{component_id}/route-templates` | headless-runtime-client | Cclc Route Templates |
| `POST` | `/api/cclc/lifecycle/requests` | headless-runtime-client | Cclc Lifecycle Submit |
| `GET` | `/api/cclc/lifecycle/transactions/{transaction_id}` | headless-runtime-client | Cclc Lifecycle Status |
| `GET` | `/api/cclc/owner` | headless-runtime-client | Cclc Owner Identity |
| `POST` | `/api/cclc/routes/requests` | headless-runtime-client | Cclc Route Submit |
| `GET` | `/api/cclc/routes/transactions/{transaction_id}` | headless-runtime-client | Cclc Route Status |
| `GET` | `/api/execution/alarms` | headless-runtime-client | Alarms |
| `POST` | `/api/execution/alarms/acknowledge-all` | headless-runtime-client | Acknowledge All |
| `GET` | `/api/execution/alarms/metrics` | headless-runtime-client | Alarm Metrics |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` | headless-runtime-client | Acknowledge Alarm |
| `POST` | `/api/execution/capability-command-previews` | headless-runtime-client | Execution Preview Capability Command |
| `POST` | `/api/execution/full-stop` | headless-runtime-client | Execution Full Stop |
| `GET` | `/api/execution/history` | headless-runtime-client | History |
| `GET` | `/api/execution/history/health` | headless-runtime-client | Historian Health |
| `POST` | `/api/execution/history/reconcile` | headless-runtime-client | Historian Reconcile |
| `POST` | `/api/execution/motion-previews` | headless-runtime-client | Execution Preview Motion |
| `POST` | `/api/execution/operations` | headless-runtime-client | Execution Submit |
| `GET` | `/api/execution/operations/{operation_id}` | headless-runtime-client | Execution Read |
| `POST` | `/api/execution/operations/{operation_id}/acknowledge-completed` | headless-runtime-client | Execution Acknowledge Completed |
| `POST` | `/api/execution/operations/{operation_id}/pause` | headless-runtime-client | Execution Pause |
| `POST` | `/api/execution/operations/{operation_id}/reset` | headless-runtime-client | Execution Reset |
| `POST` | `/api/execution/operations/{operation_id}/resume` | headless-runtime-client | Execution Resume |
| `GET` | `/api/execution/operations/{operation_id}/runtime-scale` | headless-runtime-client | Execution Get Runtime Scale |
| `POST` | `/api/execution/operations/{operation_id}/runtime-scale` | headless-runtime-client | Execution Set Runtime Scale |
| `POST` | `/api/execution/pick-place/plans/pick` | headless-runtime-client | Execution Plan Pick |
| `POST` | `/api/execution/pick-place/plans/place` | headless-runtime-client | Execution Plan Place |
| `POST` | `/api/execution/pick-place/sequences/pick` | headless-runtime-client | Execution Start Pick |
| `POST` | `/api/execution/pick-place/sequences/place` | headless-runtime-client | Execution Start Place |
| `GET` | `/api/execution/pick-place/sequences/{sequence_id}` | headless-runtime-client | Execution Pick Place Sequence |
| `GET` | `/api/execution/status` | headless-runtime-client | Execution Status |
| `GET` | `/api/execution/stop-fault` | headless-runtime-client | Execution Stop Fault |
| `POST` | `/api/execution/stop-fault/recover` | headless-runtime-client | Execution Recover Stop Fault |
| `POST` | `/api/execution/stops` | headless-runtime-client | Execution Stop |
| `GET` | `/api/files/browse` | files | Browse Filesystem |
| `GET` | `/api/health` |  | Health |
| `POST` | `/api/manager/apply` | headless-runtime-client | Manager Apply |
| `GET` | `/api/manager/apply/status` | headless-runtime-client | Manager Apply Status |
| `GET` | `/api/manager/diff` | headless-runtime-client | Manager Diff |
| `GET` | `/api/manager/graph` | headless-runtime-client | Manager Graph |
| `GET` | `/api/manager/status` | headless-runtime-client | Manager Status |
| `GET` | `/api/node-environment` | headless-runtime-client | Node Environment |
| `GET` | `/api/nodes/environment` | node-environment | Get Node Environment |
| `GET` | `/api/program/applied` | headless-runtime-client | Program Applied |
| `POST` | `/api/program/apply` | composer-draft | Apply Draft |
| `POST` | `/api/program/cycle/reset` | headless-runtime-client | Program Cycle Reset |
| `GET` | `/api/program/cycle/status` | headless-runtime-client | Program Cycle Status |
| `POST` | `/api/program/discard` | composer-draft | Discard Draft |
| `GET` | `/api/program/draft` | composer-draft | Read Draft |
| `POST` | `/api/program/draft/mutations` | composer-draft | Mutate Draft |
| `POST` | `/api/program/new-draft` | composer-draft | New Draft |
| `POST` | `/api/program/open-draft` | composer-draft | Open Draft |
| `POST` | `/api/program/revert` | composer-draft | Revert Draft |
| `GET` | `/api/program/runs` | headless-runtime-client | Program Runs |
| `POST` | `/api/program/runs` | headless-runtime-client | Program Run Start |
| `GET` | `/api/program/runs/{program_run_id}` | headless-runtime-client | Program Run Read |
| `POST` | `/api/program/runs/{program_run_id}/stop` | headless-runtime-client | Program Run Stop |
| `POST` | `/api/program/save` | composer-draft | Save Draft |
| `POST` | `/api/program/save-as` | composer-draft | Save Draft As |
| `GET` | `/api/program/workspace` | composer-draft | Read Workspace |
| `GET` | `/api/runtime` | headless-runtime-client | Runtime |
| `GET` | `/api/scenarios` | headless-runtime-client | Scenarios |
| `GET` | `/api/scenarios/current` | composer-draft | Derive Current Scenario |
| `POST` | `/api/scenarios/load` | composer-draft | Load Scenario |
| `GET` | `/api/scene` | headless-runtime-client | Scene |
| `GET` | `/api/scene/object-meshes/resolve` | headless-scene-mesh-client | Resolve Scene Object Mesh |
| `GET` | `/api/scene/object-meshes/{asset_hash}.glb` | headless-scene-mesh-client | Get Scene Object Mesh |
| `POST` | `/api/scene/pointcloud/observations` | headless-pointcloud-observer-client | Publish Pointcloud |
| `GET` | `/api/scene/pointcloud/status` | headless-pointcloud-observer-client | Pointcloud Status |
| `GET` | `/api/scene/snapshot/render` | headless-viewer-snapshot-client | Render Snapshot |
| `GET` | `/api/scene/snapshot/status` | headless-viewer-snapshot-client | Snapshot Status |
| `POST` | `/api/scene/snapshot/upload/{request_id}` | headless-viewer-snapshot-client | Upload Snapshot |
| `POST` | `/api/scene/transactions` | headless-runtime-client | Scene Commit |
| `GET` | `/api/scene/urdf/manifest` | structural-urdf | Get Manifest |
| `GET` | `/api/scene/urdf/models/{model_key}/collision-meshes/{link_name}/{collision_index}` | structural-urdf | Get Model Collision Mesh |
| `GET` | `/api/scene/urdf/models/{model_key}/meshes/{link_name}/{visual_index}` | structural-urdf | Get Model Mesh |
| `GET` | `/api/source-model` | headless-runtime-client | Source Model |
| `DELETE` | `/api/{unavailable_path}` |  | Unavailable Api |
| `GET` | `/api/{unavailable_path}` |  | Unavailable Api |
| `PATCH` | `/api/{unavailable_path}` |  | Unavailable Api |
| `POST` | `/api/{unavailable_path}` |  | Unavailable Api |
| `PUT` | `/api/{unavailable_path}` |  | Unavailable Api |
