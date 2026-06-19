<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd /workspace
    python3 scripts/regen-public-contracts.py
  Last regenerated: 2026-06-19 12:07:56 UTC
  Generator source: scripts/regen-public-contracts.py:gen_rest_api
-->

# Composer REST API
FastAPI app, served by `python -m manyforge_composer ...` on the port specified by `--port` (default `9000`). The live schema is available at `/openapi.json` and `/docs` while Composer is running.

OpenAPI version: `3.1.0`. Title: `ManyForge Composer`.

## Routes

| Method | Path | Tag | Summary |
|---|---|---|---|
| `DELETE` | `/api/assistant/bridge/principal-binding/{principal}` | assistant | Unbind Principal |
| `PUT` | `/api/assistant/bridge/principal-binding/{principal}` | assistant | Bind Principal |
| `POST` | `/api/assistant/bridge/tools/{tool_id}` | assistant | Execute Bridge Tool |
| `POST` | `/api/assistant/chat` | assistant | Chat |
| `GET` | `/api/assistant/modes/{mode}` | assistant | Describe Assistant Mode |
| `GET` | `/api/assistant/proposals` | assistant | List Proposals |
| `POST` | `/api/assistant/proposals/stage` | assistant | Stage Proposals |
| `POST` | `/api/assistant/proposals/{proposal_id}/discard` | assistant | Discard Proposal |
| `POST` | `/api/assistant/proposals/{proposal_id}/materialize` | assistant | Materialize Proposal |
| `GET` | `/api/assistant/providers` | assistant | List Providers |
| `GET` | `/api/assistant/requests/{request_id}` | assistant | Get Request Status |
| `POST` | `/api/assistant/requests/{request_id}/cancel` | assistant | Cancel Request |
| `GET` | `/api/assistant/tools` | assistant | List Tools |
| `POST` | `/api/assistant/tools/inspect-isaac-scene` | assistant | Inspect Isaac Scene |
| `POST` | `/api/assistant/tools/propose-scene-object-nodes` | assistant | Propose Scene Nodes |
| `POST` | `/api/assistant/tools/propose-scene-objects` | assistant | Propose Scene Objects |
| `GET` | `/api/capabilities` | skills | Get Capabilities |
| `GET` | `/api/catalog` | catalog | Get Catalog |
| `GET` | `/api/catalog/{entry_id}` | catalog | Get Catalog Entry |
| `POST` | `/api/cycle/acknowledge-intervention-for-retry` | cycle | Acknowledge Intervention For Retry |
| `POST` | `/api/cycle/reset` | cycle | Reset Cycle |
| `POST` | `/api/cycle/run` | cycle | Run Cycle |
| `GET` | `/api/cycle/status` | cycle | Get Status |
| `POST` | `/api/cycle/stop` | cycle | Stop Cycle |
| `PUT` | `/api/cycle/velocity-scale` | cycle | Set Velocity Scale |
| `POST` | `/api/debug/collision/compare-positions` | debug | Debug Collision Compare Positions |
| `GET` | `/api/debug/collision/compare-trajectory/{move_id}` | debug | Debug Collision Compare Trajectory Waypoint |
| `GET` | `/api/debug/export` | debug | Debug Export |
| `GET` | `/api/debug/export/ros2-medkit` | debug | Debug Export Ros2 Medkit |
| `GET` | `/api/debug/scene-trace` | debug | Debug Scene Trace |
| `POST` | `/api/debug/signals/publish` | debug | Debug Publish Signal |
| `GET` | `/api/deployment` | program | Get Deployment |
| `PUT` | `/api/deployment` | program | Update Deployment |
| `GET` | `/api/deployment/full` | program | Get Full Deployment |
| `POST` | `/api/deployment/load` | program | Load Deployment |
| `POST` | `/api/deployment/save` | program | Save Deployment |
| `GET` | `/api/deployment/saved/full` | program | Get Saved Full Deployment |
| `POST` | `/api/direct/abort` | direct | Abort Intervention |
| `POST` | `/api/direct/cartesian-jog` | direct | Cartesian Jog |
| `POST` | `/api/direct/execute` | direct | Execute Intervention |
| `POST` | `/api/direct/heartbeat` | direct | Renew Intervention Lease |
| `POST` | `/api/direct/joint-jog` | direct | Joint Jog |
| `GET` | `/api/direct/pending-recovery` | direct | Get Pending Delegated Recovery |
| `POST` | `/api/direct/promote` | direct | Promote Intervention Blackboard |
| `GET` | `/api/direct/status` | direct | Get Intervention Status |
| `GET` | `/api/execution/blackboard` | execution | Get Blackboard Snapshot |
| `GET` | `/api/execution/channels` | execution | Get Execution Channels |
| `GET` | `/api/execution/digest` | execution | Get Execution Digest |
| `GET` | `/api/execution/history` | execution | Get Execution History |
| `GET` | `/api/files/browse` | files | Browse Filesystem |
| `GET` | `/api/health` |  | Health |
| `GET` | `/api/infra/heartbeat` | infra | Infra Heartbeat |
| `GET` | `/api/infra/model-endpoints` | infra | Infra Model Endpoints |
| `GET` | `/api/infra/status` | infra | Infra Status |
| `GET` | `/api/kernel/events` | kernel | Get Kernel Events |
| `GET` | `/api/kernel/metrics` | kernel | Get Kernel Metrics |
| `GET` | `/api/kernel/snapshot` | kernel | Get Kernel Snapshot |
| `GET` | `/api/program` | program | Get Program |
| `POST` | `/api/program/load` | program | Load Program |
| `GET` | `/api/program/runtime-overrides` | program | Get Runtime Override Status |
| `POST` | `/api/program/runtime-overrides` | program | Apply Runtime Override |
| `POST` | `/api/program/runtime-overrides/activate` | program | Activate Runtime Overrides |
| `POST` | `/api/program/runtime-overrides/clear` | program | Clear Runtime Overrides |
| `GET` | `/api/program/runtime-overrides/materialized-tree` | program | Get Materialized Runtime Override Tree |
| `POST` | `/api/program/runtime-overrides/promote` | program | Promote Runtime Overrides |
| `POST` | `/api/program/runtime-overrides/revalidate` | program | Revalidate Runtime Overrides |
| `DELETE` | `/api/program/runtime-overrides/{node_name}` | program | Remove Runtime Override |
| `POST` | `/api/program/save` | program | Save Program |
| `GET` | `/api/program/saved` | program | Get Saved Program |
| `GET` | `/api/program/saved/tree` | program | Get Saved Program Tree |
| `GET` | `/api/program/tree` | program | Get Program Tree |
| `PUT` | `/api/program/tree` | program | Update Program Tree |
| `POST` | `/api/program/validate` | program | Validate Program Tree |
| `POST` | `/api/robot-control/candidates` | robot-control | Create Candidate |
| `GET` | `/api/robot-control/candidates/{candidate_id}` | robot-control | Get Candidate |
| `POST` | `/api/robot-control/candidates/{candidate_id}/execute` | robot-control | Execute Candidate |
| `POST` | `/api/robot-control/candidates/{candidate_id}/revert` | robot-control | Revert Candidate |
| `POST` | `/api/runtime/external/{provider_id}` | runtime | Update External Runtime Status |
| `GET` | `/api/runtime/status` | runtime | Runtime Status |
| `GET` | `/api/scene/diagnostics` | scene | Get Scene Diagnostics |
| `POST` | `/api/scene/edit` | scene | Edit Scene |
| `GET` | `/api/scene/frames` | scene | Get Scene Frames |
| `POST` | `/api/scene/frames/gizmo-size` | scene | Set Frame Gizmo Size |
| `POST` | `/api/scene/frames/visibility` | scene | Set Frame Visibility |
| `POST` | `/api/scene/gripper/command` | scene | Command Gripper |
| `GET` | `/api/scene/object-meshes/{asset_hash}.glb` | scene-object-meshes | Get Scene Object Mesh |
| `POST` | `/api/scene/objects/frame-visibility` | scene | Set Object Frame Visibility |
| `POST` | `/api/scene/objects/gizmo-size` | scene | Set Object Axes Gizmo Size |
| `POST` | `/api/scene/pose-sources/apply` | scene | Apply Pose Source Updates |
| `POST` | `/api/scene/pose-sources/{pose_source_id}/snapshot` | scene | Update Pose Source Snapshot |
| `POST` | `/api/scene/preview-link-poses` | scene | Preview Link Poses |
| `POST` | `/api/scene/program-resource/update` | scene | Update Program Scene Resource |
| `GET` | `/api/scene/snapshot` | scene | Get Scene Snapshot |
| `POST` | `/api/scene/snapshot/upload/{request_id}` | scene-snapshot | Upload Snapshot |
| `GET` | `/api/scene/snapshots/{snapshot_id}` | scene | Get Published Scene Snapshot |
| `GET` | `/api/scene/state` | scene | Get Scene State |
| `POST` | `/api/scene/sync-authored` | scene | Sync Authored Scene |
| `GET` | `/api/scene/urdf/manifest` | scene-urdf | Get Manifest |
| `GET` | `/api/scene/urdf/meshes/{link_name}/{visual_index}` | scene-urdf | Get Mesh |
| `GET` | `/api/scene/urdf/models/{model_key}/meshes/{link_name}/{visual_index}` | scene-urdf | Get Model Mesh |
| `POST` | `/api/session/apply` | program | Apply Session |
| `POST` | `/api/simulation/entity-state` | simulation | Set Entity State |
| `POST` | `/api/simulation/reset` | simulation | Reset Simulation |
| `POST` | `/api/simulation/state` | simulation | Set Simulation State |
| `POST` | `/api/simulation/step` | simulation | Step Simulation |
| `GET` | `/api/skills` | skills | Get Skills |
