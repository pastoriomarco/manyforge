<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py structural-rest
  Last regenerated: 2026-10-28 20:22:58 UTC
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
| `GET` | `/api/deployment/assistant-profiles` | composer-deployment-draft | Read Assistant Profiles |
| `GET` | `/api/deployment/component-catalog` | composer-deployment-draft | Read Component Catalog |
| `POST` | `/api/deployment/discard` | composer-deployment-draft | Discard |
| `POST` | `/api/deployment/draft/apply` | composer-deployment-draft | Apply Authored Candidate |
| `POST` | `/api/deployment/draft/components` | composer-deployment-draft | Add Components |
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
| `POST` | `/api/execution/cell-recover` | headless-runtime-client | Execution Cell Recover |
| `POST` | `/api/execution/cell-recover/abort` | headless-runtime-client | Execution Cell Recover Abort |
| `GET` | `/api/execution/control-state` | headless-runtime-client | Execution Control State |
| `GET` | `/api/execution/diagnostics` | headless-runtime-client | Execution Diagnostics |
| `POST` | `/api/execution/force-close` | headless-runtime-client | Execution Force Close |
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
| `POST` | `/api/manager/apply/acknowledge` | headless-runtime-client | Manager Apply Acknowledge |
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

## Route notes

### `POST /api/activation-preparations`

With `origin.kind: sealed_canonical_empty` this prepares Scenario Close. Close is a sequence the caller runs in order: Reset the Program cycle; detach every carried object (`POST /api/scene/transactions`, `detach_carried_object_at_current_pose`); disable each component and stop its processes; then prepare, preview and apply this canonical-empty configuration (`scene_initialization: preserve`). Only then New or Load. The Apply keeps the Scene, so this preparation is refused with 409 `scenario_close_attachment_present` while any carried object remains: `details.carried_objects` names each `scene_resource_id` with its carrier (`controllable_component_id`, `command_target_id`). Nothing is prepared; detach, then prepare again. Only the first preparation after a Force close step `close_operations`, before its step `end`, keeps an attachment (a part whose detach was refused because the robot's process is already gone). The Composer Close runs this sequence.

### `POST /api/execution/cell-recover`

Recover cell, one server-side composite. Send `request_context` with `request_id`, `source_label` and an optional `correlation_id`; every fence is read from the current owners. An optional `expected_incident_ids` lists the open incident IDs the operator was shown: an open incident not listed refuses the press `cclc_incident_changed` before any step (its details name it), each acknowledgement is pinned to the incident read (one closed meanwhile, with nothing else open, is an ordinary Recover), and the press's later Recovers acknowledge none (an incident opened meanwhile refuses them `cclc_incident_open`); absent, every open incident is acknowledged. It acknowledges open component incidents, attempts the current cell Stop-fault latch recovery, and submits Recover for components whose previously recovered cause was still reported at the press and remains so. It then tries to enable again each component whose Active target an incident retired, until Active is restored or another target replaces it (CCLC's `active_target_retired_by_incident`; a component with an open incident but no such retirement is only acknowledged) (Recover, Prepare, Enable; `component_rearm_*` reasons, for example the emergency stop still engaged), listing it in `recovered_components` on success. Each step waits within that component's own lifecycle allowance (the timeout CCLC accepted it with), so a press may outlast one HTTP call: send the same `request_id` again to wait for its result. A robot moved from its own interface on an idle cell is re-armed here too (Recover; the driver re-drives Prepare and Enable), although a motion command no longer needs it: its vendor adapter re-engages the robot at rest before the trajectory. A robot still moving is refused `component_rearm_required` here. A refused recovery still returns a JSON result: read `outcome` (`done` or `not_recovered`), `reason.code`, `acknowledged_incidents`, `latch_cleared` and `recovered_components` rather than treating HTTP success as recovery. Example reason codes (not exhaustive): `stop_fault_recovery_unavailable`, `stop_fault_recovery_closing` (Scenario Close keeps the latch; the latch recovery was refused, but incident acknowledgements earlier in the same press may already have happened), `stop_fault_latch_replaced`, `cclc_incident_changed`, `cell_state_unavailable`, `stale_request_owner_epoch` (CCLC restarted during the press; the refused step had no effect, earlier ones are listed; press again), `cell_recover_in_progress` and `cell_recover_outcome_unknown` (a step may have reached its owner: inspect and reconcile, do not assume nothing happened). `done` does not prove that every robot is ready to move. Repeating a `request_id` returns the stored result, or `cell_recover_in_progress`, without reading state again; this replay is in memory per Headless process, bounded, and lost on restart or eviction. A resend with `replay_only: true` never starts a press: an unknown `request_id` (Headless restarted or the result was evicted) returns `cell_recover_outcome_unknown` with `details.replay` `request_id_unknown`. Use a new `request_id` for each new press. While a press runs, Scenario Close's lifecycle target retirement is refused `recover_cell_in_progress`; wait for the press or abort it (`POST /api/execution/cell-recover/abort`).

### `POST /api/execution/cell-recover/abort`

Abort the running Recover cell press, the operator's voluntary action. Send only `request_context` with `request_id`, `source_label` and an optional `correlation_id`. No later step of the press starts; the step in flight is not cancelled but settles within its own limit. The press then ends `not_recovered` with reason `cell_recover_aborted` (read it with its own `request_id`): its details name the step it stopped before (`before_step`), the re-enable steps done per component (`reenable_steps`, `component:recover+prepare;...`) and the components not enabled again (`not_reenabled`), next to `acknowledged_incidents`, `latch_cleared` and `recovered_components` (`before_step` `none` when the step in flight was the last and succeeded). If that step fails, its own reason is reported, with `details.aborted` `true` and the same lists. Abort is checked right before each owner write, so a read in progress sends nothing after it. Scenario Close is allowed again once the press has ended. With no press running the abort is refused `cell_recover_not_running`.

### `POST /api/execution/force-close`

Force close, the operator's explicit action after a Close refused for an uncertain outcome. The caller runs it in order. Step `stop` sends a cell Full Stop (`request_context` names the execution owner epoch; `expected_stop_generation` is `cell_stop_action.expected_stop_generation`) and waits briefly until the Kernel confirms it: its certificate is the newest and succeeded. Otherwise it is refused with 409 `scenario_force_close_stop_unconfirmed` (`details.reason`: `stop_pending`, `stop_superseded`, `stop_not_contained`) and the caller does nothing more. Then the caller retires the lifecycle targets (now admitted with resident operations). Step `close_operations` then moves every resident operation into history marked `cell_force_closed_by_operator`, frees its resources, clears the cell-wide motion block an unresolved operation closure left (`motion_block_cleared`; motion still needs Prepare and Enable), and records one History row (`scenario_force_closed`) naming the operations and outputs left in an unknown state. It is refused `scenario_force_close_not_closing` before the retirement and `scenario_force_close_operation_busy` while an operation is being updated (retry); a pending I/O-rule Stop response is not waited for. Orphan resources from an earlier restart are freed and their operations recorded settled, which ends that motion block too. Then the caller runs the ordinary Reset and detach while the component processes still run (a part whose detach is refused, such as when the robot's process is already gone, stays attached; the next empty preparation then skips the attachment check), disables each component where possible, stops its processes, and runs the canonical-empty Apply. On every exit, success or failure, the caller sends step `end`, which ends that attachment exception. Force close never commands an output: an output's physical state stays unknown. The cell Stop fault is kept.
