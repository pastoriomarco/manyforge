<!--
  THIS FILE IS GENERATED — do not hand-edit. Source of truth: code.
  Regenerate with:
    cd <repository-root>
    python3 scripts/regen-public-contracts.py headless-rest
  Last regenerated: 2026-09-10 23:46:35 UTC
  Generator source: scripts/regen-public-contracts.py:gen_headless_rest_api
-->

# Headless Runtime REST API

This is the independent headless projection over Manager, logical-scene, scene-runtime, Alarm Lifecycle, Historian, and the selected addressed execution owner and independent Program runner. Reads return owner-issued facts. Alarm acknowledgement, Manager and Program Apply, execution submission, no-motion preview, Program Run, explicit Program Stop, and confirmed CCLC lifecycle/route requests are attributed intents. Generic `/api/control/state` and `/api/control/commands` provide the freshness-aware REST form of the same transport-neutral control ingress used by `/ws/control`; no command is automatically replayed. The API never owns or reduces execution, Program-run, lifecycle, or controller-route state. Preview cannot dispatch, and approval uses the ordinary operation endpoint. An unconfigured or lost Program runner makes Program-owned routes unavailable and is never interpreted as idle; independently owned native operation and Stop routes remain available when their owner is configured. No condition mutation endpoint exists. `/api/health` reports only `headless_projection_ready`; robot readiness has no H0 owner and is explicitly unavailable. The alarm page defaults to 32 records and is bounded at 128, history defaults to and is bounded at 32 records, ordinary request bodies are bounded at 1 MiB, Program Apply at 17 MiB, model-attempt audit at 256 KiB, native/HTTP responses at 64 MiB, and private Program-runner responses at 32 MiB. Program diagnostic captures are separately limited to 64 KiB and asynchronously written by the existing Historian. Their producer-clock values, omissions and delivery losses are available through History/debug export and recorder health, not the high-rate control/viewer stream. Skill expansion is effect-free, selects one exact installed NodeEnvironment and requires ordinary Program/Binding validation before any subsequent execution.

## Explicit operator gripper confirmation

Submit `gripper_confirm` once through `/api/control/commands` or the same control WebSocket. Read a current `/api/control/state` freshness context and the native execution owner epoch first. The payload uses the DTO below plus `expected_execution_owner_epoch`. The transport retains `request_context.request_id` but replaces its owner/source attribution from the admitted envelope. No caller timestamp, validity duration, sensor source or automatic retry is accepted.

This requires an explicitly qualified retained `tool.gripper.confirmation.operator` binding and configured validity; maintained defaults do not enable it. It records an attributed operator statement for one completed command, not sensor proof or attachment permission. Query `gripper_confirmation` at the existing target-observation endpoint; expired evidence is trace only. Program `gripper.wait_for_status` consumes the same current evidence.

The result carries native confirmation evidence and `history_recorded`. That flag means both bounded History submissions were queued, not synchronous disk durability. A History failure cannot change native acceptance; a lost reply remains unconfirmed and must not be resent as a new physical action.

```json
{
  "$defs": {
    "CallerAttributionRequest": {
      "additionalProperties": false,
      "properties": {
        "correlation_id": {
          "anyOf": [
            {
              "type": "string"
            },
            {
              "type": "null"
            }
          ],
          "default": null,
          "title": "Correlation Id"
        },
        "request_id": {
          "title": "Request Id",
          "type": "string"
        },
        "request_owner_epoch": {
          "title": "Request Owner Epoch",
          "type": "string"
        },
        "source_label": {
          "title": "Source Label",
          "type": "string"
        }
      },
      "required": [
        "request_id",
        "request_owner_epoch",
        "source_label"
      ],
      "title": "CallerAttributionRequest",
      "type": "object"
    }
  },
  "additionalProperties": false,
  "description": "Operator input, never a caller-selected source policy or timestamp.",
  "properties": {
    "command_id": {
      "maximum": 9007199254740991,
      "minimum": 1,
      "title": "Command Id",
      "type": "integer"
    },
    "confirmed": {
      "title": "Confirmed",
      "type": "boolean"
    },
    "expected_status_revision": {
      "maximum": 9007199254740991,
      "minimum": 1,
      "title": "Expected Status Revision",
      "type": "integer"
    },
    "generation": {
      "maximum": 9007199254740991,
      "minimum": 1,
      "title": "Generation",
      "type": "integer"
    },
    "program_binding_id": {
      "pattern": "^sha256:[0-9a-f]{64}$",
      "title": "Program Binding Id",
      "type": "string"
    },
    "request_context": {
      "$ref": "#/$defs/CallerAttributionRequest"
    },
    "schema_version": {
      "const": "manyforge.gripper_confirmation_request.v1",
      "title": "Schema Version",
      "type": "string"
    },
    "target_role_id": {
      "title": "Target Role Id",
      "type": "string"
    }
  },
  "required": [
    "schema_version",
    "program_binding_id",
    "target_role_id",
    "command_id",
    "generation",
    "expected_status_revision",
    "confirmed",
    "request_context"
  ],
  "title": "GripperConfirmationRequest",
  "type": "object"
}
```

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
| `POST` | `/api/cclc/lifecycle/requests/result` |  | Cclc Lifecycle Request Result |
| `GET` | `/api/cclc/lifecycle/transactions/{transaction_id}` |  | Cclc Lifecycle Status |
| `GET` | `/api/cclc/owner` |  | Cclc Owner Identity |
| `POST` | `/api/cclc/routes/requests` |  | Cclc Route Submit |
| `POST` | `/api/cclc/routes/requests/result` |  | Cclc Route Request Result |
| `GET` | `/api/cclc/routes/transactions/{transaction_id}` |  | Cclc Route Status |
| `GET` | `/api/cell/authoring-admission` |  | Authoring Admission |
| `GET` | `/api/component-overview` |  | Component Overview |
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
| `GET` | `/api/execution/history/discovery` |  | History Discovery |
| `GET` | `/api/execution/history/export` |  | History Export |
| `GET` | `/api/execution/history/health` |  | Historian Health |
| `POST` | `/api/execution/history/reconcile` |  | Historian Reconcile |
| `GET` | `/api/execution/history/{sequence}` |  | History Detail |
| `GET` | `/api/execution/identity` |  | Execution Identity |
| `POST` | `/api/execution/manual-handover` |  | Execution Read Manual Handover |
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
| `POST` | `/api/program/diagnostics` |  | Program Diagnostic |
| `GET` | `/api/program/diagnostics/health` |  | Program Diagnostic Health |
| `POST` | `/api/program/model-attempts` |  | Program Model Attempt |
| `GET` | `/api/program/runs` |  | Program Runs |
| `POST` | `/api/program/runs` |  | Program Run Start |
| `GET` | `/api/program/runs/{program_run_id}` |  | Program Run Read |
| `POST` | `/api/program/runs/{program_run_id}/blackboard-values` |  | Write Runtime Blackboard |
| `POST` | `/api/program/runs/{program_run_id}/full-stop` |  | Program Run Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/manual-handover` |  | Program Run Manual Handover |
| `POST` | `/api/program/runs/{program_run_id}/manual-result` |  | Stage Manual Result |
| `POST` | `/api/program/runs/{program_run_id}/motion-stop` |  | Program Run Motion Stop |
| `POST` | `/api/program/runs/{program_run_id}/reset` |  | Program Run Reset Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/resume-full-stop` |  | Program Run Resume Full Stop |
| `POST` | `/api/program/runs/{program_run_id}/resume-motion` |  | Program Run Motion Resume |
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
| `POST` | `/api/scene/transactions/status` |  | Scene Transaction Status |
| `GET` | `/api/skills` |  | Skills |
| `POST` | `/api/skills/expand` |  | Expand Skill |
| `GET` | `/api/source-model` |  | Source Model |
| `GET` | `/api/source-model-context` |  | Source Model Context |
| `GET` | `/api/viewer-model/resource` |  | Viewer Model Resource |
| `GET` | `/api/viewer-model/urdf` |  | Viewer Model Urdf |
