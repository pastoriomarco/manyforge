<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py structural-rest
  Last regenerated: 2026-10-22 07:21:43 UTC
  Generator source: scripts/regen-public-contracts.py:gen_rest_api
-->

# Composer REST API

This is the structural, authoring, and thin runtime-client Composer surface retained during the composable-cell runtime cutover. Catalog reads, sandboxed artifact browsing, structural URDF assets, health, and the strict revision-fenced Program Draft/Saved workflow remain available. When Headless and the independent Program runner are configured, Composer also forwards scenario, Applied, Program Run, status, explicit Program Stop, and confirmed CCLC lifecycle/route requests without owning their state. Generic `/api/control/state` and `/api/control/commands` expose the same state-conditioned ingress used by the versioned `/ws/control` session; callers submit once and observe backend-owned lifecycle state. Complete product launch and remaining provider-specific capabilities remain unavailable. Build-owned skills are discovered and expanded into ordinary Program trees through `/api/skills`; expansion does not edit Draft or execute a command. The catch-all `/api/{unavailable_path}` operations return the typed runtime-unavailable response for former product routes.

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Composer (replaceable client)`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `POST` | `/api/activation-preparations` | headless-runtime-client | Prepare Canonical Empty |
| `POST` | `/api/assistant/bridge/tools/{tool_id}` | assistant-draft-callback | Invoke Draft Tool |
| `GET` | `/api/assistant/modes` | headless-runtime-client | Assistant Modes |
| `GET` | `/api/assistant/modes/{mode_id}` | headless-runtime-client | Assistant Mode |
| `POST` | `/api/authoring/reset-apply-contexts` | composer-authoring-workspace | Record Reset Apply |
| `GET` | `/api/authoring/submission-outcome` | composer-authoring-workspace | Read Submission Outcome |
| `GET` | `/api/authoring/workspace` | composer-authoring-workspace | Read Authoring Workspace |
| `GET` | `/api/catalog` | catalog | Get Catalog |
| `GET` | `/api/catalog/{entry_id}` | catalog | Get Catalog Entry |
| `GET` | `/api/cclc/components/{component_id}` | headless-runtime-client | Cclc Component |
| `POST` | `/api/cclc/components/{component_id}/process/stop` | headless-runtime-client | Stop Component Process |
| `GET` | `/api/cclc/components/{component_id}/process/stop-offer` | headless-runtime-client | Component Process Stop Offer |
| `POST` | `/api/cclc/components/{component_id}/process/stop/result` | headless-runtime-client | Component Process Stop Result |
| `GET` | `/api/cclc/components/{component_id}/route-templates` | headless-runtime-client | Cclc Route Templates |
| `POST` | `/api/cclc/lifecycle/requests` | headless-runtime-client | Cclc Lifecycle Submit |
| `POST` | `/api/cclc/lifecycle/requests/result` | headless-runtime-client | Cclc Lifecycle Request Result |
| `GET` | `/api/cclc/lifecycle/transactions/{transaction_id}` | headless-runtime-client | Cclc Lifecycle Status |
| `GET` | `/api/cclc/owner` | headless-runtime-client | Cclc Owner Identity |
| `POST` | `/api/cclc/routes/requests` | headless-runtime-client | Cclc Route Submit |
| `GET` | `/api/cclc/routes/transactions/{transaction_id}` | headless-runtime-client | Cclc Route Status |
| `POST` | `/api/cclc/targets/retire` | headless-runtime-client | Retire Lifecycle Targets |
| `POST` | `/api/cclc/targets/retire/result` | headless-runtime-client | Lifecycle Target Retirement Result |
| `GET` | `/api/cclc/targets/retirement-offer` | headless-runtime-client | Lifecycle Target Retirement Offer |
| `POST` | `/api/cclc/targets/settlement` | headless-runtime-client | Lifecycle Owner Settlement |
| `GET` | `/api/component-overview` | headless-runtime-client | Component Overview |
| `GET` | `/api/composer/settings` | composer-settings | Read Settings |
| `PUT` | `/api/composer/settings` | composer-settings | Replace Settings |
| `DELETE` | `/api/composer/settings/profiles/{name}` | composer-settings | Delete Profile |
| `POST` | `/api/composer/settings/profiles/{name}` | composer-settings | Save Profile |
| `POST` | `/api/composer/settings/profiles/{name}/load` | composer-settings | Load Profile |
| `POST` | `/api/composer/settings/reset-damaged` | composer-settings | Reset Damaged |
| `POST` | `/api/composer/settings/restore-defaults` | composer-settings | Restore Defaults |
| `POST` | `/api/control/commands` | headless-runtime-client | Submit Control Command |
| `GET` | `/api/control/state` | headless-runtime-client | Control Projection |
| `POST` | `/api/cycle/recovery-actions` | headless-runtime-client | Cycle Recovery Action |
| `GET` | `/api/cycle/status` | headless-runtime-client | Program Cycle Status |
| `GET` | `/api/debug/export` | headless-runtime-client | Debug Export |
| `GET` | `/api/debug/export/ros2-medkit` | headless-runtime-client | Normalized Trace Export |
| `POST` | `/api/deployment/discard` | composer-deployment-draft | Discard |
| `POST` | `/api/deployment/draft/apply` | composer-deployment-draft | Apply Authored Candidate |
| `POST` | `/api/deployment/draft/controlled-stop` | composer-deployment-draft | Update Controlled Stop |
| `POST` | `/api/deployment/draft/motion-target-limit` | composer-deployment-draft | Update Motion Target Limit |
| `POST` | `/api/deployment/draft/mutations` | composer-deployment-draft | Mutate Draft |
| `POST` | `/api/deployment/draft/preview` | composer-deployment-draft | Preview Authored Candidate |
| `POST` | `/api/deployment/open-draft` | composer-deployment-draft | Open Draft |
| `POST` | `/api/deployment/revert` | composer-deployment-draft | Revert |
| `POST` | `/api/deployment/save` | composer-deployment-draft | Save Draft |
| `POST` | `/api/deployment/save-as` | composer-deployment-draft | Save Draft As |
| `GET` | `/api/deployment/workspace` | composer-deployment-draft | Read Workspace |
| `GET` | `/api/execution/alarms` | headless-runtime-client | Alarms |
| `POST` | `/api/execution/alarms/acknowledge-all` | headless-runtime-client | Acknowledge All |
| `GET` | `/api/execution/alarms/metrics` | headless-runtime-client | Alarm Metrics |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` | headless-runtime-client | Acknowledge Alarm |
| `POST` | `/api/execution/capability-command-previews` | headless-runtime-client | Execution Preview Capability Command |
| `GET` | `/api/execution/control-state` | headless-runtime-client | Execution Control State |
| `GET` | `/api/execution/diagnostics` | headless-runtime-client | Execution Diagnostics |
| `POST` | `/api/execution/full-stop` | headless-runtime-client | Execution Full Stop |
| `GET` | `/api/execution/history` | headless-runtime-client | History |
| `GET` | `/api/execution/history/captures` | headless-runtime-client | History Captures |
| `GET` | `/api/execution/history/discovery` | headless-runtime-client | History Discovery |
| `GET` | `/api/execution/history/export` | headless-runtime-client | History Export |
| `GET` | `/api/execution/history/health` | headless-runtime-client | Historian Health |
| `POST` | `/api/execution/history/reconcile` | headless-runtime-client | Historian Reconcile |
| `GET` | `/api/execution/history/{sequence}` | headless-runtime-client | Historian Detail |
| `GET` | `/api/execution/identity` | headless-runtime-client | Execution Identity |
| `POST` | `/api/execution/io-signals/mock-input` | headless-runtime-client | Execution Set Mock Io Input |
| `POST` | `/api/execution/io-signals/read` | headless-runtime-client | Execution Read Io Signal |
| `POST` | `/api/execution/manual-handover` | headless-runtime-client | Execution Read Manual Handover |
| `POST` | `/api/execution/motion-previews` | headless-runtime-client | Execution Preview Motion |
| `POST` | `/api/execution/operations` | headless-runtime-client | Execution Submit |
| `GET` | `/api/execution/operations/{operation_id}` | headless-runtime-client | Execution Read |
| `POST` | `/api/execution/operations/{operation_id}/acknowledge-completed` | headless-runtime-client | Execution Acknowledge Completed |
| `POST` | `/api/execution/operations/{operation_id}/reset` | headless-runtime-client | Execution Reset |
| `POST` | `/api/execution/pick-place/plans/pick` | headless-runtime-client | Execution Plan Pick |
| `POST` | `/api/execution/pick-place/plans/place` | headless-runtime-client | Execution Plan Place |
| `GET` | `/api/execution/program-runs/{program_run_id}/admitted-speeds` | headless-runtime-client | Program Run Admitted Speeds |
| `GET` | `/api/execution/runtime-scale-control` | headless-runtime-client | Execution Get Runtime Scale Control |
| `POST` | `/api/execution/runtime-scale-control` | headless-runtime-client | Execution Set Runtime Scale Control |
| `GET` | `/api/execution/stop-fault` | headless-runtime-client | Execution Stop Fault |
| `POST` | `/api/execution/stop-fault/recover` | headless-runtime-client | Execution Recover Stop Fault |
| `POST` | `/api/execution/stops` | headless-runtime-client | Execution Stop |
| `GET` | `/api/execution/targets` | headless-runtime-client | Execution Targets |
| `GET` | `/api/files/browse` | files | Browse Filesystem |
| `GET` | `/api/health` |  | Health |
| `POST` | `/api/manager/apply` | headless-runtime-client | Apply Manager Candidate |
| `GET` | `/api/manager/apply/status` | headless-runtime-client | Manager Apply Status |
| `GET` | `/api/manager/canonical-empty-selection` | headless-runtime-client | Canonical Empty Selection |
| `POST` | `/api/manager/deployment-candidates/preview` | headless-runtime-client | Preview Manager Candidate |
| `GET` | `/api/manager/diff` | headless-runtime-client | Manager Diff |
| `GET` | `/api/manager/graph` | headless-runtime-client | Manager Graph |
| `GET` | `/api/manager/status` | headless-runtime-client | Manager Status |
| `GET` | `/api/node-environment` | headless-runtime-client | Node Environment |
| `GET` | `/api/nodes/environment` | node-environment | Get Node Environment |
| `GET` | `/api/program/applied` | headless-runtime-client | Program Applied |
| `GET` | `/api/program/applied/status` | headless-runtime-client | Program Applied Status |
| `POST` | `/api/program/apply` | composer-draft | Apply Draft |
| `POST` | `/api/program/cycle/reset` | headless-runtime-client | Program Cycle Reset |
| `GET` | `/api/program/diagnostics/health` | headless-runtime-client | Program Diagnostic Health |
| `POST` | `/api/program/discard` | composer-draft | Discard Draft |
| `GET` | `/api/program/draft` | composer-draft | Read Draft |
| `POST` | `/api/program/draft/expression-check` | composer-draft | Check Expression |
| `POST` | `/api/program/draft/mutations` | composer-draft | Mutate Draft |
| `GET` | `/api/program/draft/scene-meshes/resolve` | headless-scene-mesh-client | Resolve Program Draft Scene Mesh |
| `GET` | `/api/program/draft/scene-meshes/{asset_hash}.glb` | headless-scene-mesh-client | Get Program Draft Scene Mesh |
| `POST` | `/api/program/new-draft` | composer-draft | New Draft |
| `POST` | `/api/program/open-draft` | composer-draft | Open Draft |
| `GET` | `/api/program/persistent-values` | headless-runtime-client | Program Persistent Values |
| `POST` | `/api/program/persistent-values` | headless-runtime-client | Set Program Persistent Value |
| `POST` | `/api/program/persistent-values/clear` | headless-runtime-client | Clear Program Persistent Values |
| `POST` | `/api/program/persistent-values/reset` | headless-runtime-client | Reset Program Persistent Value |
| `POST` | `/api/program/revert` | composer-draft | Revert Draft |
| `GET` | `/api/program/runs` | headless-runtime-client | Program Runs |
| `POST` | `/api/program/runs` | headless-runtime-client | Program Run Start |
| `GET` | `/api/program/runs/{program_run_id}` | headless-runtime-client | Program Run Read |
| `POST` | `/api/program/runs/{program_run_id}/blackboard-values` | headless-runtime-client | Write Runtime Blackboard |
| `POST` | `/api/program/runs/{program_run_id}/full-stop` | headless-runtime-client | Program Run Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/manual-result` | headless-runtime-client | Stage Manual Result |
| `POST` | `/api/program/runs/{program_run_id}/motion-start` | headless-runtime-client | Program Run Motion Start |
| `POST` | `/api/program/runs/{program_run_id}/motion-stop` | headless-runtime-client | Program Run Motion Stop |
| `POST` | `/api/program/runs/{program_run_id}/node-correction-previews` | headless-runtime-client | Program Node Correction Preview |
| `POST` | `/api/program/runs/{program_run_id}/reset` | headless-runtime-client | Program Run Reset Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/resume-full-stop` | headless-runtime-client | Program Run Resume Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/resume-motion` | headless-runtime-client | Program Run Motion Resume |
| `GET` | `/api/program/runs/{program_run_id}/runtime-controls` | headless-runtime-client | Program Runtime Controls |
| `POST` | `/api/program/runs/{program_run_id}/stop` | headless-runtime-client | Program Run Stop |
| `POST` | `/api/program/save` | composer-draft | Save Draft |
| `POST` | `/api/program/save-as` | composer-draft | Save Draft As |
| `POST` | `/api/program/validate` | composer-draft | Validate Program Source |
| `GET` | `/api/program/workspace` | composer-draft | Read Workspace |
| `GET` | `/api/runtime` | headless-runtime-client | Runtime |
| `GET` | `/api/scenario-documents` | composer-scenario-documents | List Documents |
| `POST` | `/api/scenario-documents/discard` | composer-scenario-documents | Discard Scenario |
| `POST` | `/api/scenario-documents/export` | composer-scenario-documents | Export Document |
| `POST` | `/api/scenario-documents/load` | composer-scenario-documents | Load Document |
| `POST` | `/api/scenario-documents/new` | composer-scenario-documents | New Document |
| `POST` | `/api/scenario-documents/save` | composer-scenario-documents | Save Document |
| `GET` | `/api/scenarios/current` | headless-runtime-client | Current Scenario |
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
| `GET` | `/api/scene/viewer/status` | headless-viewer-observation-client | Viewer Status |
| `GET` | `/api/skills` | headless-runtime-client | Skills |
| `POST` | `/api/skills/expand` | headless-runtime-client | Expand Skill |
| `GET` | `/api/source-model` | headless-runtime-client | Source Model |
| `DELETE` | `/api/{unavailable_path}` |  | Unavailable Api |
| `GET` | `/api/{unavailable_path}` |  | Unavailable Api |
| `PATCH` | `/api/{unavailable_path}` |  | Unavailable Api |
| `POST` | `/api/{unavailable_path}` |  | Unavailable Api |
| `PUT` | `/api/{unavailable_path}` |  | Unavailable Api |
