<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py structural-rest
  Last regenerated: 2026-11-25 12:04:59 UTC
  Generator source: scripts/regen-public-contracts.py:gen_rest_api
-->

# Composer REST API

This is the structural, authoring, and thin runtime-client Composer surface. Catalog reads, sandboxed artifact browsing, structural URDF assets, health, and the strict revision-fenced Program Draft/Saved workflow remain available. When Headless and the independent Program runner are configured, Composer also forwards scenario, Applied, Program Run, status, Stops, component actions and the server-run Apply/Load/New requests without owning their state. Generic `/api/control/state` and `/api/control/commands` expose the same state-conditioned ingress used by the versioned `/ws/control` session; callers submit once and observe backend-owned lifecycle state. Complete product launch and remaining provider-specific capabilities remain unavailable. Build-owned skills are discovered and expanded into ordinary Program trees through `/api/skills`; expansion does not edit Draft or execute a command. The catch-all `/api/{unavailable_path}` operations return the typed runtime-unavailable response for former product routes.

**Real robot.** A real robot's address is a site fact (`device_addresses` in the `manyforge.site.v2` site settings file); a Scenario names only its key (`configuration.device_address_ref` of the machine owner). Component states, faults, the cell latch and readiness are readable here; the site file, robot power, network and e-stop are the operator's. See [Real robot setup](../operations/REAL_ROBOT_SETUP.md).

The live schema is available at `/openapi.json` and `/docs` while the API process is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Composer (replaceable client)`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `POST` | `/api/assistant/bridge/tools/{tool_id}` | assistant-draft-callback | Invoke Draft Tool |
| `GET` | `/api/assistant/modes` | headless-runtime-client | Assistant Modes |
| `GET` | `/api/assistant/modes/{mode_id}` | headless-runtime-client | Assistant Mode |
| `POST` | `/api/assistant/requests` | assistant-provider-client | Submit |
| `GET` | `/api/assistant/requests/{request_id}` | assistant-provider-client | Status |
| `POST` | `/api/assistant/requests/{request_id}/cancel` | assistant-provider-client | Cancel |
| `POST` | `/api/authoring/reset-apply-contexts` | composer-authoring-workspace | Record Reset Apply |
| `GET` | `/api/authoring/submission-outcome` | composer-authoring-workspace | Read Submission Outcome |
| `GET` | `/api/authoring/workspace` | composer-authoring-workspace | Read Authoring Workspace |
| `GET` | `/api/catalog` | catalog | Get Catalog |
| `GET` | `/api/catalog/{entry_id}` | catalog | Get Catalog Entry |
| `GET` | `/api/cell/latch` | headless-runtime-client | Cell Latch |
| `GET` | `/api/components` | headless-runtime-client | Components |
| `POST` | `/api/components/actions` | headless-runtime-client | Component Group Action |
| `POST` | `/api/components/recover-enable` | headless-runtime-client | Component Recover Enable Group |
| `POST` | `/api/components/{component_id}/actions` | headless-runtime-client | Component Action |
| `POST` | `/api/components/{component_id}/recover-enable` | headless-runtime-client | Component Recover Enable |
| `GET` | `/api/components/{component_id}/recover-enable/preflight` | headless-runtime-client | Component Recovery Preflight |
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
| `POST` | `/api/deployment/apply` | composer-deployment-draft | Apply Deployment |
| `GET` | `/api/deployment/assistant-profiles` | composer-deployment-draft | Read Assistant Profiles |
| `GET` | `/api/deployment/component-catalog` | composer-deployment-draft | Read Component Catalog |
| `POST` | `/api/deployment/discard` | composer-deployment-draft | Discard |
| `POST` | `/api/deployment/draft/components` | composer-deployment-draft | Add Components |
| `POST` | `/api/deployment/draft/components/remove` | composer-deployment-draft | Remove Component |
| `POST` | `/api/deployment/draft/components/replace-tool` | composer-deployment-draft | Replace Tool |
| `POST` | `/api/deployment/draft/controlled-stop` | composer-deployment-draft | Update Controlled Stop |
| `POST` | `/api/deployment/draft/motion-target-limit` | composer-deployment-draft | Update Motion Target Limit |
| `POST` | `/api/deployment/draft/mutations` | composer-deployment-draft | Mutate Draft |
| `GET` | `/api/deployment/draft/tool-meshes/{asset_hash}.glb` | composer-tool-parameters | Get Tool Mesh |
| `GET` | `/api/deployment/effective` | composer-deployment-draft | Effective Values |
| `POST` | `/api/deployment/open-draft` | composer-deployment-draft | Open Draft |
| `POST` | `/api/deployment/preview` | composer-deployment-draft | Preview Deployment |
| `POST` | `/api/deployment/revert` | composer-deployment-draft | Revert |
| `POST` | `/api/deployment/revert/preview` | composer-deployment-draft | Preview Revert |
| `POST` | `/api/deployment/tool-parameters/export` | composer-tool-parameters | Export Tool |
| `POST` | `/api/deployment/tool-parameters/preview` | composer-tool-parameters | Preview Tool |
| `GET` | `/api/deployment/workspace` | composer-deployment-draft | Read Workspace |
| `GET` | `/api/execution/alarms` | headless-runtime-client | Alarms |
| `POST` | `/api/execution/alarms/acknowledge-all` | headless-runtime-client | Acknowledge All |
| `GET` | `/api/execution/alarms/metrics` | headless-runtime-client | Alarm Metrics |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` | headless-runtime-client | Acknowledge Alarm |
| `POST` | `/api/execution/capability-command-previews` | headless-runtime-client | Execution Preview Capability Command |
| `GET` | `/api/execution/control-state` | headless-runtime-client | Execution Control State |
| `GET` | `/api/execution/diagnostics` | headless-runtime-client | Execution Diagnostics |
| `POST` | `/api/execution/halt` | headless-runtime-client | Execution Halt |
| `GET` | `/api/execution/history` | headless-runtime-client | History |
| `GET` | `/api/execution/history/captures` | headless-runtime-client | History Captures |
| `GET` | `/api/execution/history/discovery` | headless-runtime-client | History Discovery |
| `GET` | `/api/execution/history/export` | headless-runtime-client | History Export |
| `GET` | `/api/execution/history/health` | headless-runtime-client | Historian Health |
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
| `GET` | `/api/execution/pick-place/plans/{job_id}` | headless-runtime-client | Execution Read Planning Job |
| `POST` | `/api/execution/pick-place/plans/{job_id}/cancel` | headless-runtime-client | Execution Cancel Planning Job |
| `POST` | `/api/execution/pick-place/plans/{job_id}/release` | headless-runtime-client | Execution Release Planning Job |
| `GET` | `/api/execution/program-runs/{program_run_id}/admitted-speeds` | headless-runtime-client | Program Run Admitted Speeds |
| `GET` | `/api/execution/runtime-scale-control` | headless-runtime-client | Execution Get Runtime Scale Control |
| `POST` | `/api/execution/runtime-scale-control` | headless-runtime-client | Execution Set Runtime Scale Control |
| `POST` | `/api/execution/stop-all` | headless-runtime-client | Execution Stop All |
| `POST` | `/api/execution/stops` | headless-runtime-client | Execution Stop |
| `GET` | `/api/execution/targets` | headless-runtime-client | Execution Targets |
| `GET` | `/api/files/browse` | files | Browse Filesystem |
| `GET` | `/api/health` |  | Health |
| `GET` | `/api/io` | headless-runtime-client | Io Signals |
| `POST` | `/api/io/{signal_id}/set` | headless-runtime-client | Set Io Output |
| `POST` | `/api/manager/apply/acknowledge` | headless-runtime-client | Manager Apply Acknowledge |
| `GET` | `/api/manager/apply/status` | headless-runtime-client | Manager Apply Status |
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
| `GET` | `/api/program/readiness` | headless-runtime-client | Program Readiness |
| `POST` | `/api/program/revert` | composer-draft | Revert Draft |
| `GET` | `/api/program/runs` | headless-runtime-client | Program Runs |
| `POST` | `/api/program/runs` | headless-runtime-client | Program Run Start |
| `GET` | `/api/program/runs/{program_run_id}` | headless-runtime-client | Program Run Read |
| `GET` | `/api/program/runs/{program_run_id}/blackboard-values` | headless-runtime-client | Program Runtime Blackboard Values |
| `POST` | `/api/program/runs/{program_run_id}/blackboard-values` | headless-runtime-client | Write Runtime Blackboard |
| `POST` | `/api/program/runs/{program_run_id}/manual-result` | headless-runtime-client | Stage Manual Result |
| `POST` | `/api/program/runs/{program_run_id}/motion-start` | headless-runtime-client | Program Run Motion Start |
| `POST` | `/api/program/runs/{program_run_id}/motion-stop` | headless-runtime-client | Program Run Motion Stop |
| `POST` | `/api/program/runs/{program_run_id}/node-correction-previews` | headless-runtime-client | Program Node Correction Preview |
| `POST` | `/api/program/runs/{program_run_id}/reset` | headless-runtime-client | Program Run Discard Halt |
| `POST` | `/api/program/runs/{program_run_id}/resume-halt` | headless-runtime-client | Program Run Resume Halt |
| `POST` | `/api/program/runs/{program_run_id}/resume-motion` | headless-runtime-client | Program Run Motion Resume |
| `GET` | `/api/program/runs/{program_run_id}/runtime-controls` | headless-runtime-client | Program Runtime Controls |
| `POST` | `/api/program/scene-preparations` | headless-runtime-client | Program Scene Prepare |
| `POST` | `/api/program/validate` | composer-draft | Validate Program Source |
| `GET` | `/api/program/workspace` | composer-draft | Read Workspace |
| `GET` | `/api/runtime` | headless-runtime-client | Runtime |
| `GET` | `/api/scenario-documents` | composer-scenario-documents | List Documents |
| `POST` | `/api/scenario-documents/discard` | composer-scenario-documents | Discard Scenario |
| `POST` | `/api/scenario-documents/export` | composer-scenario-documents | Export Document |
| `POST` | `/api/scenario-documents/save` | composer-scenario-documents | Save Document |
| `GET` | `/api/scenario-documents/upgrade` | composer-scenario-documents | Check Upgrade |
| `POST` | `/api/scenario-documents/upgrade` | composer-scenario-documents | Upgrade Document |
| `POST` | `/api/scenario/load` | composer-scenario-documents | Load Document |
| `POST` | `/api/scenario/new` | composer-scenario-documents | New Document |
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

## Change requests

This section says how to send a request that changes something, so that it is accepted, and what to do when it is refused. It is generated from the request models.

### Where to send

Composer serves every path on this page at `http://<host>:9000` (the default `--port` of `python -m manyforge_composer`). On the development workbench, Composer listens on port 9020; while the workbench runs the development GUI, the GUI address on port 3000 forwards `/api` too. Bodies are JSON (`Content-Type: application/json`). Some routes use camelCase names (`requestKey`), others snake_case names (`request_key`). Send each name exactly as this page shows it for that route; do not convert between the two styles. A body with an unknown or missing field is refused with `invalid_request` (HTTP 422).

### Terms

| Term | Meaning |
|---|---|
| change request | A request that changes a Draft, the authoring workspace or the cell. |
| fence field | A body field that repeats a value the client read. The server compares it with the current value. If they differ, the server refuses the request and changes nothing. |
| request key | 32 random hex characters made by the client. One request key is one attempt. A new attempt gets a new request key. |
| refusal | An error answer with a typed `detail.code`. |
| Apply command | Apply, Revert, Load or New. The server answers as soon as it accepts the command. The command then runs; the client reads its row to see when it ends and how. |
| row | The server's record of one Apply command: its state while it runs and its result when it has ended. |

### How to send a change request

1. Read the current values. The table *Fence fields* says where each value is read. Read them just before you send.
2. Make a new request key: 32 random hex characters (for example Python `secrets.token_hex(16)`).
3. Build the body: the route's own fields (see `/openapi.json`) plus every fence field the table *Fenced requests* lists for the route.
4. Send the request once.
5. Read the answer. A 2xx answer means it was done. For an Apply command, 2xx means only *accepted*: read its row until the row has ended (see *Apply commands*).
6. If the answer is a refusal, look up `detail.code` in *Refusals* and do what it says. To try again, start at step 1: read the values again, make a new request key and build a new body. Never send an old body again after a refusal.
7. If you got no answer at all (timeout, lost connection), you do not know whether it was done. Do not make a new attempt yet. Check first (see *No answer*).

### Fence fields

| Field | Meaning | Read from |
|---|---|---|
| `expectedAdmissionGeneration` | The cell's authoring admission counter. | `admission_generation` of `GET /api/cell/authoring-admission` on the Headless API (port 9010; Composer does not serve it), or `shell.authoring_admission.admission_generation` of a `/ws/control` snapshot |
| `applicationStartId` | This start of ManyForge (16 lowercase hex). It changes when ManyForge restarts. | `application_start_id` of `GET /api/runtime` |
| `requestKey` | The request key of this attempt. | Make a new one: 32 random hex characters |
| `expectedWorkspaceEpoch` | The authoring workspace instance. | `authoringWorkspace.workspaceEpoch` of `GET /api/program/workspace` or `GET /api/deployment/workspace` |
| `expectedWorkspaceRevision` | The authoring workspace revision. | `authoringWorkspace.workspaceRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace` |
| `draftId` | The Draft this route edits (see the Draft column). | Draft column Program: `authoringWorkspace.programDraftId`; Deployment: `authoringWorkspace.deploymentDraftId` (same reads as the workspace fields) |
| `expectedDraftRevision` | The revision of that Draft. | Draft column Program: `authoringWorkspace.programDraftRevision`; Deployment: `authoringWorkspace.deploymentDraftRevision` |
| `deploymentDraftId` | The Deployment Draft. | `authoringWorkspace.deploymentDraftId` of `GET /api/program/workspace` or `GET /api/deployment/workspace` |
| `expectedDeploymentDraftRevision` | The Deployment Draft revision. | `authoringWorkspace.deploymentDraftRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace` |
| `request_key` | The request key of this attempt. | Make a new one: 32 random hex characters |
| `application_start_id` | This start of ManyForge (16 lowercase hex). | `application_start_id` of `GET /api/runtime` |
| `expected_admission_generation` | The cell's authoring admission counter. | `admission_generation` of `GET /api/cell/authoring-admission` on the Headless API (port 9010; Composer does not serve it), or `shell.authoring_admission.admission_generation` of a `/ws/control` snapshot |
| `expected_draft_id` | The Program Draft. | `authoringWorkspace.programDraftId` of `GET /api/program/workspace` or `GET /api/deployment/workspace` |
| `expected_draft_revision` | The Program Draft revision. | `authoringWorkspace.programDraftRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace` |
| `expected_authoring_workspace` | The whole authoring workspace identity. | The whole `authoringWorkspace` object, unchanged |
| `request_context` | An object: `request_key` (the request key of this attempt) and `source_label` (free text naming the sender). | Put a new request key in `request_key` |

The Draft column of the next table says which Draft `draftId` and `expectedDraftRevision` name. *Program and Deployment*: `draftId` is the Program Draft and `deploymentDraftId` the Deployment Draft.

### Fenced requests

| Method | Path | Draft | Fence fields |
|---|---|---|---|
| `POST` | `/api/assistant/bridge/tools/{tool_id}` | Program | `expectedAdmissionGeneration`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/components/recover-enable` | — | `application_start_id` |
| `POST` | `/api/components/{component_id}/recover-enable` | — | `application_start_id` |
| `POST` | `/api/cycle/recovery-actions` | — | `request_key`, `application_start_id` |
| `POST` | `/api/deployment/apply` | — | `request_key` |
| `POST` | `/api/deployment/discard` | Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/deployment/draft/components` | Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/deployment/draft/components/remove` | Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/deployment/draft/components/replace-tool` | Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/deployment/draft/controlled-stop` | Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/deployment/draft/motion-target-limit` | Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/deployment/draft/mutations` | Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/deployment/open-draft` | Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/deployment/revert` | — | `request_key` |
| `POST` | `/api/execution/alarms/acknowledge-all` | — | `request_key`, `application_start_id` |
| `POST` | `/api/execution/alarms/{condition_id}/acknowledge` | — | `request_key`, `application_start_id` |
| `POST` | `/api/execution/capability-command-previews` | — | `application_start_id`, `request_context` |
| `POST` | `/api/execution/halt` | — | `request_context` |
| `POST` | `/api/execution/manual-handover` | — | `application_start_id`, `request_context` |
| `POST` | `/api/execution/motion-previews` | — | `application_start_id`, `request_context` |
| `POST` | `/api/execution/operations` | — | `application_start_id`, `request_context` |
| `POST` | `/api/execution/operations/{operation_id}/acknowledge-completed` | — | `application_start_id`, `request_context` |
| `POST` | `/api/execution/operations/{operation_id}/reset` | — | `application_start_id`, `request_context` |
| `POST` | `/api/execution/runtime-scale-control` | — | `application_start_id`, `request_context` |
| `POST` | `/api/execution/stop-all` | — | `request_context` |
| `POST` | `/api/execution/stops` | — | `application_start_id`, `request_context` |
| `POST` | `/api/manager/apply/acknowledge` | — | `application_start_id`, `request_context` |
| `POST` | `/api/program/apply` | Program | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/program/cycle/reset` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/discard` | Program | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/program/draft/mutations` | Program | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/program/new-draft` | Program | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/program/open-draft` | Program | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/program/persistent-values` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/persistent-values/clear` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/persistent-values/reset` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/revert` | Program | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision` |
| `POST` | `/api/program/runs` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/runs/{program_run_id}/blackboard-values` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/runs/{program_run_id}/manual-result` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/runs/{program_run_id}/motion-start` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/runs/{program_run_id}/node-correction-previews` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/runs/{program_run_id}/reset` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/runs/{program_run_id}/resume-halt` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/runs/{program_run_id}/resume-motion` | — | `request_key`, `application_start_id` |
| `POST` | `/api/program/scene-preparations` | — | `request_key`, `application_start_id`, `expected_admission_generation` |
| `POST` | `/api/scenario-documents/discard` | Program and Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision`, `deploymentDraftId`, `expectedDeploymentDraftRevision` |
| `POST` | `/api/scenario-documents/export` | Program and Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision`, `deploymentDraftId`, `expectedDeploymentDraftRevision` |
| `POST` | `/api/scenario-documents/save` | Program and Deployment | `expectedAdmissionGeneration`, `applicationStartId`, `requestKey`, `expectedWorkspaceEpoch`, `expectedWorkspaceRevision`, `draftId`, `expectedDraftRevision`, `deploymentDraftId`, `expectedDeploymentDraftRevision` |
| `POST` | `/api/scenario-documents/upgrade` | — | `request_key` |
| `POST` | `/api/scenario/load` | — | `request_key` |
| `POST` | `/api/scenario/new` | — | `request_key` |
| `POST` | `/api/scene/transactions` | Program | `application_start_id`, `expected_admission_generation`, `expected_draft_id`, `expected_draft_revision`, `expected_authoring_workspace`, `request_context` |

### Example bodies

**Program Draft (Discard the Program)** — `POST /api/program/discard`:

```json
{
  "draftId": "draft-1f6a3c8e9b2d4a7f8e0c5b3d2a1f9e46",
  "expectedDraftRevision": 5,
  "expectedAdmissionGeneration": 7,
  "applicationStartId": "3f9a00000000ffff",
  "requestKey": "9c0e5d2a7b4f41e8a3d6c1b0f2e48a17",
  "expectedWorkspaceEpoch": "workspace-0d4c9e2b8f7a4c11b5e3a9d6f0c2b871",
  "expectedWorkspaceRevision": 12
}
```

Where each value came from:

- `draftId`: `authoringWorkspace.programDraftId` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedDraftRevision`: `authoringWorkspace.programDraftRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedAdmissionGeneration`: `admission_generation` of `GET /api/cell/authoring-admission` on the Headless API (port 9010; Composer does not serve it), or `shell.authoring_admission.admission_generation` of a `/ws/control` snapshot
- `applicationStartId`: `application_start_id` of `GET /api/runtime`
- `requestKey`: Make a new one: 32 random hex characters
- `expectedWorkspaceEpoch`: `authoringWorkspace.workspaceEpoch` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedWorkspaceRevision`: `authoringWorkspace.workspaceRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace`

**Deployment Draft (Discard the Deployment)** — `POST /api/deployment/discard`:

```json
{
  "draftId": "deployment-draft-5b7e1c9a2d4f4e60a8b3c7d1e9f0a246",
  "expectedDraftRevision": 3,
  "expectedAdmissionGeneration": 7,
  "applicationStartId": "3f9a00000000ffff",
  "requestKey": "9c0e5d2a7b4f41e8a3d6c1b0f2e48a17",
  "expectedWorkspaceEpoch": "workspace-0d4c9e2b8f7a4c11b5e3a9d6f0c2b871",
  "expectedWorkspaceRevision": 12
}
```

Where each value came from:

- `draftId`: `authoringWorkspace.deploymentDraftId` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedDraftRevision`: `authoringWorkspace.deploymentDraftRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedAdmissionGeneration`: `admission_generation` of `GET /api/cell/authoring-admission` on the Headless API (port 9010; Composer does not serve it), or `shell.authoring_admission.admission_generation` of a `/ws/control` snapshot
- `applicationStartId`: `application_start_id` of `GET /api/runtime`
- `requestKey`: Make a new one: 32 random hex characters
- `expectedWorkspaceEpoch`: `authoringWorkspace.workspaceEpoch` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedWorkspaceRevision`: `authoringWorkspace.workspaceRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace`

**Scenario document (Save the Scenario)** — `POST /api/scenario-documents/save`:

```json
{
  "draftId": "draft-1f6a3c8e9b2d4a7f8e0c5b3d2a1f9e46",
  "expectedDraftRevision": 5,
  "deploymentDraftId": "deployment-draft-5b7e1c9a2d4f4e60a8b3c7d1e9f0a246",
  "expectedDeploymentDraftRevision": 3,
  "expectedWorkspaceEpoch": "workspace-0d4c9e2b8f7a4c11b5e3a9d6f0c2b871",
  "expectedWorkspaceRevision": 12,
  "expectedAdmissionGeneration": 7,
  "applicationStartId": "3f9a00000000ffff",
  "requestKey": "9c0e5d2a7b4f41e8a3d6c1b0f2e48a17",
  "directory": "/workspace/scenarios",
  "name": "my_cell"
}
```

Where each value came from:

- `draftId`: `authoringWorkspace.programDraftId` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedDraftRevision`: `authoringWorkspace.programDraftRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `deploymentDraftId`: `authoringWorkspace.deploymentDraftId` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedDeploymentDraftRevision`: `authoringWorkspace.deploymentDraftRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedWorkspaceEpoch`: `authoringWorkspace.workspaceEpoch` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedWorkspaceRevision`: `authoringWorkspace.workspaceRevision` of `GET /api/program/workspace` or `GET /api/deployment/workspace`
- `expectedAdmissionGeneration`: `admission_generation` of `GET /api/cell/authoring-admission` on the Headless API (port 9010; Composer does not serve it), or `shell.authoring_admission.admission_generation` of a `/ws/control` snapshot
- `applicationStartId`: `application_start_id` of `GET /api/runtime`
- `requestKey`: Make a new one: 32 random hex characters
- `directory`: this route’s own field (see `/openapi.json`)
- `name`: this route’s own field (see `/openapi.json`)

**Apply command (New)** — `POST /api/scenario/new`:

```json
{
  "discard_unsaved": false,
  "request_key": "9c0e5d2a7b4f41e8a3d6c1b0f2e48a17"
}
```

Where each value came from:

- `discard_unsaved`: `true` discards an unsaved Draft; with `false` New is refused while a Draft is unsaved
- `request_key`: Make a new one: 32 random hex characters

### Refusals

A refusal has the HTTP status of the error and this body: `{"detail": {"code": ..., "message": ..., "producer": ...}}`. Decide by `detail.code`, never by `message`. When `detail.effect` is `"none"`, the request changed nothing. Without that member, the request may have changed something: read the state before you try again.

| `detail.code` | Meaning | What the client does |
|---|---|---|
| `composer_workspace_stale` | The authoring workspace changed since you read it. `detail.workspace` holds the current `authoringWorkspace`. | Read the values again, make a new request key, send a new body. |
| `composer_draft_id_stale` | The Program Draft was replaced (Open, New, Load). | Read the values again, make a new request key, send a new body. |
| `composer_draft_revision_stale` | The Program Draft changed since you read it. | Read the values again, make a new request key, send a new body. |
| `deployment_draft_stale` | The Deployment Draft changed since you read it. | Read the values again, make a new request key, send a new body. |
| `scene_transaction_draft_stale` | The Program Draft changed since you read it. | Read the values again, make a new request key, send a new body. |
| `stale_generation` | The authoring admission counter moved. | Read the values again, make a new request key, send a new body. |
| `stale_application_start_id` | ManyForge restarted. Every value read before the restart is old. | Read all values again, from `GET /api/runtime` on; make a new request key, send a new body. |
| `start_identity_required` | The application start id is missing. | Add it and send again with a new request key. |
| `request_key_required` | The request key is missing. | Add a new request key and send again. |
| `request_key_invalid` | The request key is not 32 hex characters. | Make a new request key and send again. |
| `invalid_request` | The body does not match the route's schema (a missing or misspelled field, the other name style, a wrong type). `detail.details` names each bad field. | Fix the body as `detail.details` says; use a new request key. |
| `authoring_not_ready` | The cell does not allow this change now (a Program run, a manual operation or an unfinished Apply). | Do not resend at once. Wait until the cell is idle, then start again at step 1. |
| `apply_in_progress` | Another Apply command is running. | Wait until `apply_commands.in_progress` is null, then start again at step 1. |

### No answer

- Apply command: send the same body again with the same request key. The server does not run it twice: it answers with the row of that request key. Or read the row (see *Apply commands*).
- Other change request on a camelCase route: read `GET /api/authoring/submission-outcome?application_start_id=...&request_key=...&expected_admission_generation=...` with the values you sent. `outcome` not null: the request was done. `outcome` null: unknown. Read the workspace and check whether your change is there before you make a new attempt.
- Any other request: read the state the request changes before you make a new attempt.

### Apply commands

These requests answer as soon as the command is accepted. The answer is `{"schema_version": "manyforge.apply_command_reply.v1", "command": <row>}`.

| Method | Path | Row `kind` | The row ends |
|---|---|---|---|
| `POST` | `/api/deployment/apply` | `deployment_apply` | when the Apply has finished |
| `POST` | `/api/deployment/revert` | `deployment_revert` | when the Revert has finished |
| `POST` | `/api/program/apply` | `program_apply` | when the Manager has accepted the Apply |
| `POST` | `/api/program/revert` | `program_revert` | when the Manager has accepted the Apply |
| `POST` | `/api/scenario/load` | `scenario_load` | when the Load has finished |
| `POST` | `/api/scenario/new` | `scenario_new` | when the New has finished |

Read the rows in `apply_commands` of `GET /api/deployment/workspace` (also in the `/ws/control` snapshot `shell`). `apply_commands` has `in_progress` (the command that runs now, or null) and `ended` (the last 8 ended rows, newest first). Find your row by its `request_key`.

A row has `kind`, `request_key`, `step`, `accepted_at_ns`, `end`. `end` is null while the command runs. When the command has ended, `end` has `outcome`, `code`, `details`, `finished_at_ns`. `end.outcome` is one of `succeeded`, `refused`, `failed`, `unknown`, `interrupted`; `end.code` is null exactly when the outcome is `succeeded`.

An Apply command's row also has `step_source`: what the command waits for now, or null. When `step_source.status` is `manager_apply`, it names the Manager Apply (`apply_id`, `apply_generation`). Read that Apply's details at `GET /api/manager/apply/status?apply_id=...&apply_generation=...`.

Only one Apply command runs at a time.
