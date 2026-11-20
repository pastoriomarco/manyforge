# Changelog

All notable changes to ManyForge are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Before 1.0, a minor release (0.x.0) may change public interfaces and file
formats; such changes are listed explicitly.

## Unreleased

### Added

- **A prebuilt ManyForge image.** GitHub builds the complete application
  (native programs, Python modules, the built GUI) for amd64 and arm64 on the
  release's ROS image and publishes it as
  `ghcr.io/pastoriomarco/manyforge:<VERSION>`. A user needs only Docker,
  Python 3, the image and their Scenario folders: copy the launcher kit out
  of the image and `./manyforge run [--project <folder>]` starts it with
  nothing to compile and Composer serving the GUI at port 9000;
  `./manyforge status`, `logs` and `stop` inspect and stop it. `./manyforge`
  reads product variable names (`MANYFORGE_SITE_SETTINGS`,
  `MANYFORGE_DATA_DIR`, `MANYFORGE_RUNTIME_STATE_ROOT`,
  `MANYFORGE_CONTAINER_NAME`, `MANYFORGE_START_TIMEOUT_SECONDS`); it runs as
  the container `manyforge` with its data under `.manyforge/` in the launcher
  folder, and it and the development workbench refuse to run at the same
  time, whatever their container names. Developing
  keeps the ROS image with the source checkout (`pull`, then `start`). The
  image records its source commit and ROS image digest, with build
  provenance; the licence files of the frontend's npm packages are in it,
  described in `THIRD_PARTY_NOTICES_MANYFORGE_IMAGE.md`.
- **Release images publish themselves.** Pushing a `vX.Y.Z` tag runs the ROS
  image workflow (which only adds the version tag when its inputs are
  unchanged); its success builds and publishes the ManyForge image of that
  tagged commit. Both refuse a tag other than `v<VERSION>` on the commit built.
- **`./scripts/compose-dev.sh logs`** shows the workbench container's log,
  passing its options to `docker logs`.

### Changed

- **Install options renumbered**: 1 runs the prebuilt image, 2 pulls the ROS
  image and builds from the checkout, 3 builds the ROS image from source, 4 is
  the operator launcher.
- **A named built frontend runs without Vite.** `dev-workbench.py serve
  --composer-static-dir` now serves only that build from Composer, as operator
  mode does.
- **`start --project` also accepts a project in a project folder**, by its
  absolute host path, besides `examples/`.

### Fixed

- **xArm: Enable after an e-stop works the first time, and the control loop
  no longer stalls.** The vendor hardware plugin switched every controller off
  and on by itself from its control loop at each change of the robot's
  readiness, blocking the loop for about 1 s each time, racing ManyForge's
  Enable ("already active") and switching the trajectory controller on without
  an Enable. Its requests now go to a stand-in in the execution host that
  answers at once without effect, and ManyForge switches the controllers
  itself: off when a status report shows the robot not ready (e-stop, error,
  takeover), on only at Enable. The 3 s wait for the plugin's cycle at Enable
  and after a TCP offset is gone; during a TCP offset in a Program the
  controllers stay on. The vendor driver is unchanged.
- **A Stop during the xArm's wait for a status report after a re-arm or a TCP
  offset** now ends with `component_action_interrupted_by_stop`.
- **E-stop warning:** a pressed e-stop shows as a warning notice with a link to
  Components.

## 0.3.1 – 2026-11-19

### Added

- **A ready-made ROS image.** GitHub builds the workbench's ROS image for
  amd64 and arm64 (Jetson AGX Thor and Orin, JetPack 7.2.1) and publishes it
  as `ghcr.io/pastoriomarco/manyforge-ros`; `./scripts/compose-dev.sh pull`
  uses it instead of building it locally (about an hour). Building from
  source stays available (`build`). The image holds only ROS and
  dependencies; ManyForge's own programs still build at first start.
- **Third-party notices.** `THIRD_PARTY_NOTICES.md` lists every component the
  image redistributes with its licence as its owner declares it; the owners'
  licence files are in the image under `/usr/share/licenses/manyforge-image/`.
- **Nightly ROS CI.** The ROS integration and Mock-lane suites run nightly on
  the published image.
- **Overview cards** at the top of the README.

### Changed

- **Operator mode builds its frontend from the checkout.** Composer serving
  the built GUI (port 9000) no longer falls back to a frontend baked into the
  image: the workbench build step builds it from the mounted source (only when
  it changed), and an operator start without a built frontend is refused with
  the missing file named. `--composer-static-dir` still overrides it.
- **ROS image 1.85 GB smaller** (4.07 GB): files get their owner where they
  are created and source trees are build-only mounts; same content.
- **The debug export's motion trace reaches back about three times further
  (OD-314).** Controller samples are kept as compact rows in the same 1 MiB:
  2,340 samples, about 15 s at 150 Hz for one arm (about 8 s for two),
  instead of about 5 s. Recording a sample no longer allocates memory or
  copies unexported fields; the export format is unchanged. A debug export
  file may now be up to 5 MiB (was 3 MiB).
- **The development workbench builds its native programs in one tree.** The
  execution host, worker, Manager and both Python modules build together, so
  a clean native build compiles 42 % fewer files (about 21 % faster on a
  Jetson AGX Thor). Programs are built Release with debug symbols, so crash
  backtraces name functions and lines.
- **Faster execution-host answers.** `targets` re-verifies the Stop metadata
  from disk only when a file changed; the Program Runner asks the host for
  Reset-position availability once per published status instead of two or
  more times, and not at all for run readiness; the host no longer clears a
  1 MiB buffer per request; the controller-state callback allocates less.
  All host threads have names, visible in traces.
- **Composer:** the 3D viewer encodes each frame once per delivery, and the
  Program graph recolours on status updates without recomputing its layout.
- **Development image:** based on Ubuntu 24.04 (the same base and compiler
  as the ROS image), instead of Debian 12.

## 0.3.0 – 2026-11-18

### Breaking changes

- **Manager Apply records carry only the fence's component scope (OD-281).**
  In `GET /api/manager/status` (and the Manager status stream's `manager`
  section) `apply.records[].fence.scope` is now
  `{"controllable_component_ids": [...]}`; its `domains`, `bindings` and
  `resources` are gone (nothing read them; the Manager's Apply journal keeps
  them), and `apply.records[].prepared_activation` is `null` on every record
  except the one of the Apply that installed the active configuration (the
  only one read; the Apply journal keeps every receipt, and
  `GET /api/manager/apply/status` still reports each Apply's prepared
  activation identity). The `manager` section stays within the status frame's
  409,600 bytes, the limit a default host's socket allows. A cell whose
  Manager data exceeds it cannot be loaded: Load, Apply, Revert and Program
  activation are refused with `manager_status_section_oversized` ("Manager
  data too large to send") instead of `manager_projection_unavailable`, and
  the Manager tile is offline with that reason instead of
  `source_freshness_unknown`. When the Manager is not answering (no new
  status tick for 2 s), its tile is offline with `manager_status_unavailable`
  instead of waiting with `source_stale` (OD-284); a reconnect to a Manager
  whose loop is held no longer counts as an answer.
- **Deployment Apply, Revert, Load and New answer when they are accepted.**
  `POST /api/deployment/apply`, `/api/deployment/revert`, `/api/scenario/load`
  and `/api/scenario/new` require a `request_key` and reply
  `manyforge.apply_command_reply.v1` (the command's row) once their checks
  that refuse before any effect have passed; those refusals keep their reply.
  The outcome, with its typed code and the component rows, is the row's
  `end`, published as `apply_commands` in the Composer's control state
  (`manyforge.composer_headless_state.v4`) and in
  `GET /api/deployment/workspace` (`manyforge.composer_deployment_workspace.v2`).
  The same `request_key` sent again returns the same row and runs nothing.
  An activation preparation refused after acceptance now ends the row
  `refused` instead of a 409 reply. *Integrators:* send a `request_key` and
  read the outcome from `apply_commands`; Load's reply no longer carries the
  workspace, its path or diagnostics (read the workspace).
- **Program Apply and Program Revert answer when they are accepted.**
  `POST /api/program/apply` and `/api/program/revert` reply
  `manyforge.apply_command_reply.v1` (kinds `program_apply`,
  `program_revert`) instead of `{status, headless, workspace}` with 202/200.
  Their row ends once the Manager accepted the Apply (or it was already
  applied); the ended row's `step_source` names that Manager Apply
  (`apply_id`, `apply_generation`). They now hold the one Apply slot, so one
  sent during another Apply, Load or New is refused `apply_in_progress` at
  once. Refusals found while preparing (for example
  `scenario_revert_requires_deployment`) end the row `refused` instead of a
  409 reply.
- **Program commands answer when they are accepted.** Execute, group Run and
  Resume, handover, Execute after Halt, Discard Halt, Reset, Program-value
  writes, manual results, recovery actions and stored-value edits reply
  `manyforge.program_command_reply.v1` (the command's row) on the Headless API
  and through the Composer, instead of the run projection,
  `manyforge.program_cycle_reset_result.v1`,
  `manyforge.program_runtime_control_result.v2`,
  `manyforge.cycle_recovery_result.v3` or `manyforge.persistent_key_edit.v2`.
  The step and the outcome are the row in `runner_commands` of
  `GET /api/cycle/status`; the run Execute started is read there too. The
  receipt is `manyforge.execution_command_receipt.v3`: it always names the
  `request_key` (WebSocket commands too) and carries the runner's row in
  `command`; the control WebSocket is `manyforge.execution_control.v4`. The
  Headless API no longer limits its wait for the runner's answer to 30 s.
  *Integrators:* read outcomes from the status, not the reply.
- **Activation preparation answers when it is accepted.**
  `POST /api/activation-preparations` requires a `request_key` and replies
  `manyforge.activation_preparation_reply.v1` (its row, with
  `captured_input_digest`); its step and end are published as
  `activation_preparation` in `GET /api/cycle/status`, and the prepared
  activation is read with `GET /api/activation-preparations/{captured_input_digest}`
  once the row ended `succeeded`. The preparer's own 120 s limit stays.
- **The Composer no longer gives up on a running command after its Headless
  time limit.** Runner commands, stored-value edits, control commands,
  activation preparation and the Apply's Manager submissions wait for their
  answer with only the connection bounded; the Apply's Program Reset is
  followed to the end of its row, with no Apply bound; the Composer follows
  an activation preparation through its published status, with no 30 s limit.
- **Waits on the execution host and the Program runner end at the true
  answer (OD-214).** The Headless API limits only the connection to the
  execution host (commands, Stops, reads and cell-admission claims); a
  request then waits for the host's answer instead of failing
  `execution_owner_unavailable` or an unknown claim after 10 s while the host
  completes it. The Program runner does the same for its cell-admission
  claims (Execute, Reset) and for what it sends off the Program tick: output
  and valve writes, the pick/place planning request, Execute's run capture,
  the handover's read and Reset's position reset. Its requests made from the
  Program tick (motion and gripper starts, Set TCP, cancels, Program-value
  publication, I/O reads, rule activation and the others) keep their 30 s
  limit. A closed connection still gives the unknown outcomes. A Reset of a
  running Program stays in its `stopping` step until the run settles, instead
  of ending in recovery required after 30 s. A stopped node whose state cannot
  be read waits until it can be (or until Reset), instead of failing
  `stopped_node_state_unreadable` after 30 s. The Headless API's read of the
  cycle state before it forwards a command waits for the runner's answer.
  After a committed Apply, the initial scene is prepared once the authoring
  admission opens, instead of failing `program_scene_preparation_failed`
  after 30 s.
- **Every accepted New publishes the end of its Scene clear (OD-239).** In
  `scene_clear` of `GET /api/cycle/status`, an Apply without a Program whose
  clear cannot begin now ends `failed` (reason `scene_clear_failed`, with the
  typed cause in `details.reason`), for example when its committed Apply or
  Applied Program cannot be read or does not match, or when no Program runner
  is configured; one replaced by a later Apply before its clear began ends
  `superseded`. Before, these Applies published nothing. A status never
  replaces the status of a later Apply. New and Load in the Composer wait
  for the clear's end with no time limit (before: the Apply's bound); the
  wait also ends when `scene_clear` names a later Apply or a direct clear,
  which replaced the Apply's status, or when the status cannot be read.
- **The Manager's state reaches the Headless API over a kept local
  connection, not through files.** The Manager no longer writes
  `manager-source.json`, `logical-scene-source.json`,
  `scene-runtime-source.json` and `viewer-pose-sessions.json`, and the
  `current/publications/` folder is gone. It serves the same state from
  memory on its own socket, `manager-status.sock` in its run root: the whole
  state when a reader connects, then each part when it changes, and a
  keepalive every 500 ms while its main loop runs. The Manager counts as
  "not answering" (`manager_status_unavailable`, which replaces
  `manager_source_stale`, `logical_scene_source_stale` and
  `scene_runtime_source_stale`) at once when the connection ends, or when its
  main loop shows no new tick for `--freshness-ms` (default 2000); Load,
  Apply and every Deployment edit stay refused meanwhile, as before. The
  codes that only file reading produced (`*_publication_time_in_future`,
  `*_receiver_age_saturated`, `*_source_identity_mismatch`) are gone. The
  runtime snapshot is now `manyforge.headless_runtime_snapshot.v3`: its
  `receiver_age_ns` is the age of the Manager's last loop tick, and
  `source_observed_at_ns` changes only when a section's content changes.
  *Integrators:* drop `--projection-root` from the Manager and the Headless
  API (it is refused as an unknown option); tools that read the four files
  must read the runtime snapshot from the Headless API instead.
- **Every start begins with an empty cell, in both launchers.** The Manager
  no longer recreates the last Applied runtime or resumes anything from a
  previous run; the operator launcher no longer restores the last Applied
  Scenario. `--project` (workbench) still works: Composer starts once the
  Manager's empty baseline is ready and Loads the project the ordinary way.
  *Integrators:* Load the Scenario after every start, or pass `--project`.
- **Any application process exit ends the application.** The Manager starts
  Headless, the Program runner and Composer itself, watches them and the
  execution host, and ends the whole application when any of them exits; it
  never restarts a process or replaces the execution host in place. A new
  host build therefore needs a full stop and start. *Integrators:* treat the
  Manager's exit as the end of the application and start it again; a process
  supervisor must not restart single ManyForge processes.
- **Cell-admission claims carry no request digest.** A claim and its release
  no longer have `request_payload_digest`; `owner_transaction_id` is an
  integer (the run number, Apply id or operation number) for Program-cycle,
  Manager-Apply and manual-operation claims and `null` for every other claim.
  `POST /api/program/authoring-transactions` takes `request_key`,
  `source_label`, `application_start_id` and `expected_admission_generation`
  only; a body with `request_payload_digest` or `owner_transaction_id` is
  refused (422). Reusing an authoring request key with the same expected
  admission generation after its claim was released returns
  `request_result_expired`, whatever the content.
- **The operator launcher's `standard` start stays in the foreground.** It
  prints `started` once every owner reports ready, then waits for the
  application's whole life and prints `stopped` when it ends. When the
  Manager ended the application because a process exited, `start` fails with
  `application_exited` (exit status 73). `stop` from another shell asks the
  running Manager to end the application. *Integrators:* run `start` under a
  terminal or service manager, not as a command that returns.
- **Operator instance layout v2** (`manyforge.cell_instance_layout.v2`):
  `data/` (saved data: persistent Program values in
  `data/program-runner/persistent-keys.v<N>.json`, Composer settings in
  `data/composer/`, the Program workspace in `data/program-workspace/`),
  `history/`, `operator/` (launch record and lock), `current/` (one start's
  running state), `previous-runs/` and `session-reports/`. A v1 instance
  (`state/` and `run/`) is refused with `cell_layout_invalid` and is neither
  opened nor converted. *Integrators:* start a new instance (another
  `--instance-id` or `--state-root`); to keep persistent Program values, copy
  the old `state/program-runner/persistent-keys.json` into the new instance's
  `data/program-runner/` as `persistent-keys.v<N>.json` (N from its
  `schema_version`) before its first start.
- **Previous runs are kept in `previous-runs/`, in both launchers.** At each
  start the previous `current/` moves to `previous-runs/<id>` before any
  process starts. Previous runs are pruned to five runs, counting the current
  one, and to 2 GiB of archived runs and older History files together, oldest
  first; the newest archived run and the current History file are never
  deleted. A development state root that still holds `previous-sessions/` (or
  any other entry the launcher does not know) is refused and left unchanged.
  *Integrators:* move `previous-sessions/` out of the state root, then start.
- **History trouble no longer blocks motion, and History has no reconcile.**
  A record the event history cannot take or write (a full queue, a failed
  write, or a record that cannot be made into a History entry) is dropped and
  counted. It no longer makes the alarm state unavailable, so it no longer
  refuses Run, Resume, Execute or recovery. While History is losing records
  it raises the alarm `manyforge.runtime.history_missing_records` ("History
  is missing records"). That alarm gates nothing, needs no acknowledgement
  and clears once History has written its queue empty again.
  `POST /api/execution/history/reconcile`, its Composer route and the CLI
  command `reconcile-historian` are removed, with the
  `manyforge.historian_reconciliation.v1` reply, the
  `historian_health_reconciled` History event
  (`manyforge.historian_health_reconciliation_event`) and the error code
  `historian_delivery_unavailable`. `GET /api/execution/history/health`
  stays: `available` is false only while History is losing records and
  `delivery_failure_count` counts dropped records. *Integrators:* stop
  calling the reconcile route; read the alarm or the History health instead.
- **Effectful HTTP requests carry a `request_key`.** Every HTTP request that
  changes something carries `request_key`: 16 random bytes as 32 hexadecimal
  characters (upper or lower case accepted; ManyForge writes lower case).
  Without it the request is refused with `request_key_required`, with an
  invalid one with `request_key_invalid`, both before any effect. A Stop
  never needs one: a Stop without a valid key is still accepted under a key
  Headless makes. Resending the same key with the same request returns the
  first answer. WebSocket commands carry no key; the control session number
  and client sequence identify them. `request_id`, `request_owner_epoch` and
  `correlation_id` are gone from every Headless API request and reply (the
  assistant provider contract keeps its own `requestOwnerEpoch`, the id of
  one AI-tab page). *Integrators:*
  generate one fresh key per new request and reuse it only to resend that
  request.
- **Requests that name an id carry the application start identity.** Each
  start has one identity, `application_start_id`: 16 lowercase hexadecimal
  characters, made by the Manager and given to every process. An HTTP request
  that names an id made during a start (operation, Stop, run, node attempt or
  Apply number) carries it, in the JSON body of a `POST` or as a query
  parameter of a `GET`. Missing, an ordinary request is refused with
  `start_identity_required`; another start's, with
  `stale_application_start_id`; a read naming another start's id answers as for
  an unknown id. An operation Stop with a missing or wrong identity is
  accepted as Stop all. Run, Stop all and Halt name no id and need none. *Integrators:* read `application_start_id` from any reply that names
  an id (or from the control session's `server_hello`) and send it back.
- **Documents name the start once, at their top level.** The per-section and
  per-record copies of the start (`projection_owner_epoch`,
  `source_owner_epoch`, `selection_source_owner_epoch`, the alarm and
  overview `owner_epoch` members, the overview basis `*_owner_epoch` members,
  `scene_owner_epoch`, `viewer_owner_epoch`) are gone from every Headless and
  Composer document. The runtime snapshot names its start as
  `application_start_id` (formerly `projection_owner_epoch`). The viewer
  snapshot upload header is `X-ManyForge-Viewer-Application-Start-Id`, and
  the snapshot request's frame names `applicationStartId`. The History
  filter `source_owner_epoch` is removed (see "History filters by start").
  *Integrators:* read the document's or the session's top-level
  `application_start_id` instead.
- **Ids made during a start are integers.** Operation, Stop and Apply numbers
  count up from 1 in each start; node attempts are numbered from 1 within
  their run. A reply that names an
  operation, Stop or run number, or a request key, also carries
  `application_start_id` and a short `display` form, for example `3f9a/op7`,
  `3f9a/stop12` or `3f9a/s4.118` (control session 4, sequence 118); the
  first four characters are the start identity's. Controller goal ids are
  16 bytes (operation number, participant ordinal, generation).
  *Integrators:* store ids as integers together with their start identity.
- **Control protocol v3 and new wire and file formats.** The control
  WebSocket's `server_hello` carries `application_start_id` and an integer
  `control_session_number` (replacing `control_session_id` and
  `receiver_incarnation`); commands no longer carry `correlation_id` or
  `expected_semantic_owner_epoch`; `resync_required` is gone. The bumped
  formats (older versions are refused):

  | Format | Before | Now |
  | --- | --- | --- |
  | `manyforge.execution_control` (WebSocket) | v2 | v3 |
  | `manyforge.execution_command` (public command) | v1 | v2 |
  | `manyforge.execution_command_receipt` | v1 | v2 |
  | `manyforge.execution_control_projection` | v1 | v2 |
  | `manyforge.execution_command` / `_response` (execution host) | v3 / v4 | v4 / v5 |
  | `manyforge.manager_command` / `_response` | v1 / v2 | v2 / v3 |
  | `manyforge.execution_event_stream` | v1 | v2 |
  | `manyforge.execution_event_page` | v2 | v3 |
  | `manyforge.execution_stop_status` | v1 | v2 |
  | `manyforge.execution_control_state` | v7 | v8 |
  | `manyforge.kernel_recovery_assessment` | v1 | v2 |
  | `manyforge.historian_event`, `_page`, `_detail`, `_event_summary`, `_summary_page` | v2 | v3 |
  | `manyforge.operation_journal_record` | v9 | v10 |
  | `manyforge.operation_history_event` | v1 | v2 |
  | `manyforge.historian_retention` | v2 | v3 |
  | `manyforge.client_incident_event` | v1 | v2 |
  | `manyforge.operator_alarm_snapshot` | v2 | v3 |
  | `manyforge.component_overview` | v2 | v3 |
  | `manyforge.applied_program_selection` | v4 | v5 |
  | `manyforge.committed_scenario_activation` | v4 | v5 |
  | `manyforge.manager_deployment_candidate_receipt` | v2 | v3 |
  | `manyforge.authoring_submission_outcome` | v1 | v2 |
  | `manyforge.composer_reset_apply_context` | v1 | v2 |
  | `manyforge.workbench_previous_session` | v1 | v2 |
  | `manyforge.cell_instance_layout` | v1 | v2 |
  | `manyforge.active_tcp_change` | v2 | v3 |
  | `manyforge.target_observation` / `_query` | v2 | v3 |
  | `manyforge.execution_identity` | v2 | v3 |
  | `manyforge.execution_observation`, `_frame` | v4 | v5 |
  | `manyforge.execution_observation_credit` | v1 | v2 |
  | `manyforge.direct_control_motion_preview` | v4 | v5 |
  | `manyforge.direct_control_capability_command_preview` | v2 | v3 |
  | `manyforge.execution_targets` | v4 | v5 |
  | `manyforge.manager_source` (Manager status section) | v1 | v2 |
  | `manyforge.composer_headless_state` | v2 | v3 |
  | `manyforge.composer_authoring_effect` | v1 | v2 |
  | `manyforge.incident_context` | v1 | v2 |
  | `manyforge.program_run_status` | v4 | v5 |
  | `manyforge.program_run_capture` | v2 | v3 |
  | `manyforge.program_run_execution_capture` | v1 | v2 |
  | `manyforge.program_run_index` | v2 | v3 |
  | `manyforge.program_run_admitted_speeds` | v1 | v2 |
  | `manyforge.program_runner_health` | v1 | v2 |
  | `manyforge.program_runner_liveness`, `_status` | v1 | v2 |
  | `manyforge.launch_instance_status` | v1 | v2 |
  | `manyforge.program_node_availability` | v1 | v2 |
  | `manyforge.program_control_state` | v2 | v3 |
  | `manyforge.program_runtime_controls` | v3 | v4 |
  | `manyforge.program_runtime_control_projection` | v9 | v10 |
  | `manyforge.program_runtime_control_result` | v1 | v2 |
  | `manyforge.cycle_recovery_offer` | v5 | v6 |
  | `manyforge.cycle_recovery_result` | v2 | v3 |
  | `manyforge.program_reset_effect_settlement` | v1 | v2 |
  | `manyforge.persistent_key_edit` | v1 | v2 |
  | `manyforge.program_diagnostic` | v4 | v5 |
  | `manyforge.program_model_attempt` | v1 | v2 |
  | `manyforge.program_failure_condition` | v1 | v2 |
  | `manyforge.program_operation_query` | v2 | v3 |
  | `manyforge.program_execution_scope` | v2 | v3 |
  | `manyforge.program_execution_selection` | v2 | v3 |
  | `manyforge.program_node_motion_attention_check` | v1 | v2 |
  | `manyforge.program_node_correction_preview`, `_intent` | v2 | v3 |
  | `manyforge.program_node_correction_preview_request` | v1 | v2 |
  | `manyforge.program_bbkey_publication` | v1 | v2 |
  | `manyforge.io_watch_request` | v2 | v3 |
  | `manyforge.io_watch_scope_release` | v1 | v2 |
  | `manyforge.mock_io_input_request` | v2 | v3 |
  | `manyforge.mock_io_node_start` | v2 | v3 |
  | `manyforge.mock_io_node_end`, `mock_io_run_pause` | v1 | v2 |
  | `manyforge.prepared_operation_participant` | v3 | v4 |
  | `manyforge.prepared_participant_stop_plan` | v1 | v2 |
  | `manyforge.resolved_participant_stop_scope_policy` | v1 | v2 |
  | `manyforge.volatile_motion_trace` | v1 | v2 |
  | `manyforge.motion_trace_file` | v1 | v2 |
  | `manyforge.headless_runtime_snapshot` | v1 | v2 |
  | `manyforge.scene_projection` | v1 | v2 |
  | `manyforge.node_environment_projection` | v2 | v3 |
  | `manyforge.source_model_projection` | v3 | v4 |
  | `manyforge.assistant_mode_projection`, `assistant_mode_index` | v2 | v3 |
  | `manyforge.manager_status_projection`, `manager_graph_projection`, `manager_diff_projection` | v1 | v2 |
  | `manyforge.operator_alarm_metrics` | v1 | v2 |
  | `manyforge.operator_alarm_motion_attention` | v3 | v4 |
  | `manyforge.operation_event_snapshot`, `operation_event_source_quality` | v1 | v2 |
  | `manyforge.source_snapshot` (source publication files) | v1 | v2 |
  | `manyforge.program_snapshot_source` | v1 | v2 |
  | `manyforge.viewer_pointcloud_status` | v2 | v3 |
  | Operation journal schema | 20 | 23 |
  | Program cycle journal schema | 10 | 11 |
  | Manager Apply journal schema | 22 | 23 |
  | History format (file `history.v<N>.sqlite3`) | 6 | 7 |
  | RoboPlan worker protocol | 13 | 14 |
  | Manager-liveness RPC (`manyforge.manager-liveness.request`, `.response`) | v2 | v3 |
  | Kernel cell-admission RPC (`MFCA`) | 2 | 3 |
  | Kernel Apply-fence RPC (`MFNKAFR`, physical / provider scopes) | 2 / 5 | 3 / 6 |
  | Selected-runtime composition RPC (`MFNSRC`) | 6 | 7 |
  | Native projection RPC (`MFNPRPC`) | 4 | 5 |
  | Digest domains `manyforge.host-io-source`, `manual_operation_admission`, `manual_program_handover` | v1 | v2 |
  | Manager Apply journal digest domains `component-manager-apply-intent`, `-apply-fence-attempt` | v2 | v3 |
  | Digest domain `cell-admission-claim-intent-digest` | 1.0 | 2.0 |
  | Execution runtime manifest (`manyforge.ros2_control_execution_runtime_manifest`) | v1 | v3 |

  Within these unreleased versions: an `MFCA` acquire and release end with a
  presence byte for the Manager-Apply facts (candidate bundle; Apply id,
  generation and outcome); every `MFNKAFR` channel request except Release ends
  with the Manager's current publication receipt; the Manager-liveness
  request carries the full publication receipt in place of its generation,
  graph and pointer digest. The Manager-liveness request, the retained
  `manager_liveness_policy` (runtime manifest v3) and the Program-runner
  liveness probe and acknowledgement no longer carry the Manager-loss
  local-safe contract id and revision; the Manager-liveness response (also
  inside `MFCA`) and the Program-runner acknowledgement no longer carry the
  receipt age or the receiver status digest, and the
  `manyforge.manager-liveness.status` digest is gone.

  New: `manyforge.application_processes.v1`, the list of processes the
  launcher hands the Manager (`current/application-processes.json`).
- **History is `history.v8.sqlite3`.** History is kept in a directory, one
  file per event format, `history.v<N>.sqlite3`; Headless takes
  `--historian-directory` instead of `--historian-database-path`. Only the
  current format's file is opened; an older file (`history.sqlite3` or a
  lower `v<N>`) is never opened, upgraded or shown, and the History panel's
  retention notes name it. An id filter (run, operation, node attempt or Stop
  number) accepts the number, its display form (`3f9a/op7`) or its full form
  (`<16 hex>/op7`). *Integrators:* export anything you need from an older
  History file with SQLite tools; ManyForge does not read it.
- **History filters by start.** Every History event belongs to one
  application start, and `application_start_id` is a filter of
  `GET /api/execution/history`, its export and the debug export: with a run,
  operation, node attempt, Stop or action span number it selects that start's
  events (an earlier start's too), and alone it selects all retained events of
  that start. A number without a named start means the current start (no
  longer refused with `start_identity_required`); a start other than the
  current one is no longer refused with `stale_application_start_id`. A
  display form (`3f9a/op7`) must carry the named start's prefix, or the
  current start's when none is named, else `historian_query_invalid` (400);
  two different starts in one query, or a malformed start, are refused as an
  invalid request (422). "Action span" now follows the same start. The run
  list (`GET /api/execution/history/discovery?kind=program_run_id`) lists one
  start's runs (`application_start_id`, default the current start) in number
  order, and `kind=application_start_id` lists the starts that have History
  events, newest first. The History panel has a Start filter (current start,
  then earlier starts). Each row of the History page and summary page
  (`manyforge.historian_page.v3`, `manyforge.historian_summary_page.v3`)
  carries `application_start_id`. The previous-session report
  (`manyforge.workbench_previous_session.v2`) carries the
  `application_start_id` of the start it describes (null with its unfinished
  operations when that start's journal could not be read); History files the
  report and the "previous session unclean" alarm events under that start, so
  an earlier start's operation shows its own events and the report about it,
  and the current start's operation with the same number shows neither. The
  filter `source_owner_epoch` is removed (refused as an unknown parameter,
  422 `invalid_request`); alarms no longer carry it as a correlation.
  *Integrators:* select one start's events with `application_start_id`
  instead of `source_owner_epoch`; the assistant tool `manyforge_debug_export`
  no longer offers `source_owner_epoch`.
- **Stored Program values are `persistent-keys.v4.json`.** The Program
  runner's register file is named by its format version,
  `persistent-keys.v<N>.json`, and the runner takes
  `--persistent-key-register-directory` instead of
  `--persistent-key-register-path`. Only the current format's file is opened
  or created; an older file (`persistent-keys.json` or a lower `v<N>`) is
  never opened, moved, rewritten or deleted. Without a current file the
  stored values start empty and the runner logs one line naming the older
  file. *Integrators:* a `persistent-keys.json` whose `schema_version` ends
  in `.v4` can be renamed to `persistent-keys.v4.json` before the next start
  to keep its values; otherwise they start empty.
- **Processes take `--application-start-id`.** Headless, the Program runner,
  Composer and the execution host require `--application-start-id <16 hex>`
  and refuse to start without it; `--owner-id` is gone. The Manager passes
  it. *Integrators:* start ManyForge only through a launcher or the Manager.
- **Three Stop levels: Stop, Stop all and Halt.** The name "Full Stop" is
  gone.
  - **Stop** stops the selected Program groups; a manual move in progress
    keeps moving and the Program tree keeps running; **Resume** releases each
    group.
  - **Stop all** stops every group and every manual operation; the tree keeps
    running; it works with no Program and a new manual move is allowed right
    after. The Program panel's **All groups** row does Stop all; its button
    is labelled **Stop**.
  - **Halt** is Stop all plus the Program frozen and new manual moves
    blocked until **Execute** or **Reset**. The **Halt** button sends Stop
    all at once, then asks whether to halt the Program. After Execute the
    group holds stay until Resume. With no Program run, Halt holds nothing.
  - A Stop is never refused. An invalid group, operation, component or
    resource Stop becomes Stop all; an invalid Halt stays a Halt.
  - A Stop that ends uncontained raises a Halt by itself; the robot's
    component shows **Stop not confirmed** (`stop_unconfirmed`, activity
    unknown, cell latched) until its controller reports it idle. The run
    becomes halted once nothing is stopping any more, also after such a Stop,
    and keeps the reason `program_run_stop_failed`; Execute is refused while
    the uncontained operation is still in the run. Reset ends the run once
    nothing of it is stopping any more, also when its own Stop fails again,
    and completes.
  - Every Stop goes from Headless straight to the execution Kernel, never
    through the Program runner.

  Renamed or removed:

  | Before | Now |
  | --- | --- |
  | `POST /api/execution/full-stop` | `POST /api/execution/stop-all` |
  | — | `POST /api/execution/halt` (new) |
  | `POST /api/program/runs/{id}/full-stop` | removed (use `POST /api/execution/halt`) |
  | `POST /api/program/runs/{id}/resume-full-stop` | `POST /api/program/runs/{id}/resume-halt` |
  | control kind `cell_full_stop` | `stop_all` |
  | — | control kind `halt` (new) |
  | control kind `program_full_stop` | removed |
  | control kinds `program_resume_full_stop`, `program_discard_full_stop` | `program_resume_halt`, `program_discard_halt` |
  | MCP `manyforge_kernel` action `full_stop` | `stop_all` (the assistant has no Halt) |
  | Program node `kernel.full_stop` | **Cell Stop**, `kernel.cell_stop`: stops every operation in the cell without holding the Program; fails with `stop_not_contained` when a robot's Stop was not contained |
  | run states `full_stopping`, `full_stopped` | `halting`, `halted` |
  | `resume_full_stop_allowed` | `resume_halt_allowed` |
  | `expected_selection_revision`, `selection_revision` | `expected_hold_revision`, `hold_revision` |
  | reason codes `program_full_stop_*` | `program_halt_*` |

  *Integrators:* use the new routes, kinds and names; a request with an old
  name is refused as unknown.
- **Stops are recorded as Stop events.** A Stop reply carries `event_id`,
  `requested`, `applied`, `widen_reason` (`none`, `unknown_target`,
  `malformed`, `key_conflict`, `stale_start`, `unreadable_kind`), `covered`,
  `hold_revision`, `stale_diagnostics` and `stale_total`. `stop_id`,
  `stop_generation`, `resolution_disposition`, `resolution_reason`,
  `joined_predecessor_stops`, `affected_operation_revisions` and Stop
  certificates are gone, and `expected_stop_generation` is gone from Stop
  targets. Operations carry `stop_event_id`; each participant carries a
  `stop` object (phase, attempt, mode, attribution, containing stage,
  escalated). A Stop event's status (`manyforge.execution_stop_status.v2`)
  has `state` `pending` or `complete` and `result` `stopped`,
  `stopped_escalated`, `some_failed` or `no_effect`. The operation journal's Stop rows are
  `stop_event_accepted` and `stop_event_completed` with payload
  `manyforge.stop_event.v1`; `manyforge.stop_acceptance_projection` and
  `manyforge.stop_terminal_projection` are removed. The History filter is
  `stop_event_id`. *Integrators:* read Stop outcomes by event id.
- **Error codes of the Stop levels.** Added: `manual_handover_program_halted`,
  `program_halt_release_stale`, `program_selection_manual_operation_active`,
  `stop_event_record_lost`, `stop_fence_degraded`,
  `stop_lane_index_exhausted`, `stop_not_contained`,
  `stop_stage_watchdog_expired` and `stop_unconfirmed`. Removed, because
  nothing produces them any more: `emergency_stop_reserve_invariant`,
  `full_stop_intent_invalid`, `io_rule_response_priority_stale`,
  `operation_journal_stop_projection_failed`,
  `operation_journal_stop_terminal_projection_capacity_exhausted`,
  `operation_journal_stop_terminal_projection_failed`,
  `operation_journal_stop_terminal_state_projection_failed`,
  `program_full_stop_invalid`, `program_full_stop_outcome_unknown`,
  `program_full_stop_reset_failed`, `program_full_stop_reset_stale`,
  `program_run_stop_fence_capacity_exhausted`,
  `program_run_stop_fence_identity_invalid`,
  `program_run_stop_fence_not_releasable`, `program_run_stop_fence_stale`,
  `resource_stop_fence_orphan_state_invalid`,
  `resource_stop_fence_scope_invalid`, `resource_stop_generation_stale`,
  `stop_acceptance_operation_revision_duplicate`,
  `stop_acceptance_operation_scope_mismatch`, `stop_acceptance_proof_invalid`,
  `stop_acceptance_result_missing`, `stop_acceptance_time_invalid`,
  `stop_certificate_changed`, `stop_certificate_id_invalid`,
  `stop_certificate_not_resident`, `stop_counter_exhausted`,
  `stop_deadline_overflow`, `stop_id_invalid`, `stop_ingress_not_normalized`,
  `stop_join_transaction_missing`, `stop_journal_projection_plan_conflict`,
  `stop_owner_generation_changed`, `stop_participant_plan_missing`,
  `stop_projection_state_invalid`, `stop_replay_result_missing`,
  `stop_scope_snapshot_invalid` and every `stop_terminal_*` code.
  *Integrators:* drop handling for the removed codes.
- **Error codes.** Added: `application_exited`, `controller_goal_id_invalid`,
  `execution_host_planned_stop_overrun`, `manager_liveness_packet_refused`,
  `manager_planned_stop_overrun`, `operation_id_invalid`,
  `operation_journal_start_invalid`, `packet_refused`,
  `participant_id_invalid`, `program_run_id_invalid`,
  `request_key_invalid`, `request_key_required`,
  `runtime_unit_launch_in_progress`, `session_selection_unavailable` and
  `start_identity_required`. Removed, because nothing produces them any
  more: `authoring_reconciliation_changes_unrelated_owner`,
  `authoring_reconciliation_evidence_mismatch`,
  `authoring_reconciliation_invalid`,
  `authoring_reconciliation_inventory_mismatch`,
  `authoring_reconciliation_manager_evidence_invalid`,
  `authoring_reconciliation_successor_invalid`,
  `captured_stop_scope_invalid`, `captured_stop_scope_not_canonical`,
  `cell_close_containment_uncertain`, `cell_close_operation_nonterminal`,
  `claim_owner_epoch_stale`, `component_process_stop_outcome_unknown`,
  `component_process_stop_owner_unavailable`,
  `component_process_stop_pending`,
  `component_process_stop_response_unavailable`,
  `component_process_stop_result_unavailable`,
  `composer_authoring_recovery_invalid`,
  `control_owner_replaced`, `control_receiver_replaced`,
  `control_sequence_rollover_busy`, `control_sequence_rollover_required`,
  `execution_host_restart_awaiting_components`,
  `execution_host_restart_settling`, `execution_target_owner_stale`,
  `manual_owner_reconciliation_not_exact`,
  `multiple_owner_role_reconciliation_unsupported`,
  `normalized_full_stop_intent_invalid`,
  `normalized_full_stop_intent_not_canonical`,
  `normalized_stop_intent_invalid`, `normalized_stop_intent_not_canonical`,
  `operation_journal_owner_epoch_stale`,
  `operator_alarm_attention_owner_incarnation_conflict`,
  `operator_alarm_attention_owner_incarnation_stale`,
  `owner_epoch_reconciliation_identity_invalid`,
  `owner_role_reconciliation_unsupported`,
  `program_cycle_reset_interrupted`, `program_reset_claim_not_current`,
  `program_runner_reconciliation_evidence_invalid`,
  `program_run_owner_stale`, `readiness_timeout`,
  `reconcile_authorization_not_exact`,
  `reconcile_current_owner_not_authorized`, `reconcile_evidence_not_exact`,
  `reconcile_inventory_not_exact`, `reconcile_parent_child_unsettled`,
  `reconcile_predecessor_unsettled`, `reconcile_replay_conflict`,
  `reconcile_shape_invalid`, `reconciliation_owner_reader_unavailable`,
  `recovery_footprint_owner_invalid`, `recovery_receipt_owner_invalid`,
  `recovery_receipt_reconcile_invalid`, `replacement_epoch_not_new`,
  `replacement_identity_invalid`, `requested_full_stop_selector_invalid`,
  `requested_full_stop_selector_not_canonical`,
  `requested_malformed_stop_selector_invalid`,
  `requested_malformed_stop_selector_not_canonical`,
  `requested_stop_selector_invalid`,
  `requested_stop_selector_not_canonical`, `request_id_missing`,
  `request_owner_epoch_stale`, `resource_orphan_mark_state_invalid`,
  `resource_orphan_reconciliation_generation_stale`,
  `resource_orphan_reconciliation_scope_noncanonical`,
  `resource_orphan_reconciliation_state_invalid`, `startup_failed` and
  `unreconciled_external_or_orphan_goal`. *Integrators:* drop handling for
  the removed codes.
- **Execution diagnostics v4.** `GET /api/execution/diagnostics` (and MCP
  `manyforge_kernel` `diagnostics`) now answers
  `manyforge.execution_status.v4`: it adds `runtime_collision_checks`, one
  row per robot with `controllable_component_id`, `check_period_ns`,
  `checks_run`, `checks_late` and `periods_skipped`. v3 is no longer sent.
  *Integrators:* accept v4.
- **The five launcher format ids are now version 2.**
  `manyforge.operator_launch_contract.v2`,
  `manyforge.native_operator_instance.v2`,
  `manyforge.operator_command_result.v2`, `manyforge.workbench_session.v2` and
  `manyforge.workbench_session_selection.v2` replace their version 1 ids
  because their contents changed in this cycle. Files and outputs with the
  version 1 ids are not accepted. *Integrators:* accept the version 2 ids.

### Added

- **The Deployment panel shows what the Manager is waiting on.** Under
  *Desired and active*, *Manager waiting on* lists each exchange with the
  execution host or the Program Runner that is still outstanding (for
  example *Execution host · status (management) · 12 s*). No wait has a time
  limit, so a stalled peer shows here; the row is absent while nothing is
  outstanding. Information only.
- **Move To orientation *Face down, source heading* (OD-301).** The TCP points
  straight down (base −Z) and turns about the vertical so its X axis follows
  the source's X axis seen from above: a part turned on the table is
  approached with the gripper turned the same way. *Face down* keeps one
  fixed heading.
- **Each Program Reset is a History row (OD-305).** The Program Runner
  records every Reset once at its end as `program_reset_finished`
  (component `manyforge.program_runner`): succeeded, refused, failed or
  unknown with its typed error, the options (*Reload initial scene*, *Reset
  robot position*), the run it reset, and, when the scene was reloaded, the
  Applied Program it used (program revision, applied record digest) and the
  logical-scene revision the verified readback left, for example *Program
  reset succeeded · reloaded initial scene (scene revision 10)*. New Headless
  route `POST /api/execution/history/program-reset-outcomes`. A lost History
  row never changes the Reset's result.

### Changed

- **Stop all and Halt are enabled only when there is something to stop
  (OD-300).** The **All groups** row's **Stop** is enabled while a manual
  move or Program motion can move; **Halt** also while a Program run is
  active. With nothing to stop both are disabled and say why; while the live
  status feed is not current both stay enabled. With no Program run, Halt no
  longer asks for confirmation: it is Stop all.

- **Add and rename initial-scene objects.** In the Scene editor, **Add object**
  (formerly *Add box*) asks for the object's name and shape (box, sphere,
  cylinder, or a mesh from the approved files). **Rename** renames the selected
  object and every reference to it — Program nodes, keys, contacts, viewer
  settings and the Deployment's pose-source links and contact inputs — as one
  Draft edit (`scene_draft_rename_object`); a name in use or not matching the
  Program rule is refused.

- **A slow execution host no longer halts motion; no Manager request to the
  host has a time limit (OD-290).** The host's launch (was 10 s for the exec
  and 10 s for its readiness), its periodic status, the Apply fence, cell
  admission, Host Apply (was the sum of the components' budgets), projection
  requests and the Manager's liveness reports (were 750 ms) now wait for the
  host's answer; the host's own replies wait for the Manager likewise. A wait
  ends early only when the host exits (`execution_host_exited`) or ManyForge
  stops (`application_stopping`). The Manager's loop never waits on the
  host: it reads each status answer at a later tick. A slow answer sends no
  Halt and no longer makes the host count as faulted (the cell stays ready).
  While one request waits, other requests to the host answer
  `execution_host_late` at once with `effect: none` (its text is now
  "Execution host reply outstanding"; details `exchange`, `waited_ms`,
  `effect`; `bound_ms` is gone), except that a request is sent behind an
  unanswered status and answered once the host replies.
  `execution_host_late_reply` is now an information line
  (`{"code":"execution_host_late_reply","exchange":…,"waited_ms":…}`, without
  `bound_ms` and `halt`) for a request still unanswered at the Manager's next
  tick, followed by `execution_host_late_reply_settled` when it is answered.
  The Halt with source `manager.execution_host_late` and its Composer text
  "Halted: the execution host was late." are gone. The host logs its
  start-up steps (`execution host start-up: …`). This replaces the late-host
  handling of the entries below.
- **An old component status report no longer closes the cell; a slow
  execution host no longer pauses the Program Runner's liveness (OD-295).**
  The Manager's 5 s limit on the age of a runtime unit's status report is
  gone: a unit converges on its first report and its identity checks, and an
  old report by itself makes nothing non-converged (Program start, the next
  node, manual moves and Apply are no longer refused for it). The Manager's
  liveness work now runs on two lanes: one renews the Program Runner on its
  heartbeat whatever the execution host does, the other renews the host's
  receivers and publishes liveness and cell readiness. Planned maintenance
  and Apply settlement post their request to the second lane and pick up its
  answer on the Apply's next step, which that lane requests when it is done;
  the Apply lane no longer waits on another lane (an Apply's admission close
  shows as a pending step, and a successful Apply's release may show as
  pending settlement for a moment). A newly started Program Runner is
  reconciled in a second renewal round, right after the host's receivers
  are. Each runtime unit's report age (`report_age_ns`, optional) is
  published in the Manager section's `runtime.observed_units` as information
  only.
- **A slow Program Runner no longer stops the run (OD-296).** The Manager's
  liveness exchange with the Program Runner has no time limit any more (was
  250 ms for the connection and 250 ms for the answer; the Runner's own
  receive limit of 200 ms is gone too). It ends on the Runner's answer, on the
  Runner refusing or closing the connection, or on ManyForge stopping; a slow
  Runner holds only the Manager's Runner lane. A Runner that is really gone
  still stops the run (`runner_loss` Stop), and the loss now carries the
  typed code `program_runner_contact_lost` (new) in the Runner-loss alarm's
  `fault_code`, which the operator panel shows as the cause; a Runner fault
  keeps its own code. An exchange still outstanding at the Manager's next
  status tick is published as information (below).
- **Commands to the Manager wait for their answer; the Manager's loop only
  ticks and supervises (OD-298).** Manager commands (scene commits, pose
  latches, Apply acceptance, every other command) and the periodic
  reconcile run on a command lane, in the same order as before; the main
  loop keeps the status tick, signals, supervision of the application's
  processes and the stop sequence. "Manager not answering" (status tick
  silent for 2 s) now means only that loop: a command held by a slow
  execution host no longer turns the Manager tile offline. The Headless
  API's command wait (was 5 s for the whole command) and the execution
  host's before-planning latch (was 5 s) now limit only the connection: a
  connected command waits for its answer, ended by the reply, by the
  Manager closing the connection (`manager_command_unavailable`, also for a
  connection closed without an answer, which was
  `manager_command_response_invalid`) or by shutdown. The Manager's own
  250 ms limits on reading a command and sending its answer are gone; those
  waits end when the client closes the connection or ManyForge stops.
  `shutdown_runtime` is answered after the stop sequence, as before. New
  optional field `runtime.outstanding_exchanges` in the Manager section (and
  so in the REST runtime snapshot and Manager status projection): each
  exchange the Manager is waiting for that was still outstanding at its
  previous status tick, as `{peer: execution_host | program_runner,
  channel, exchange, since_ns}` (`since_ns` on the clock of
  `observed_at_ns`); information only, nothing acts on it.
- **Scene editor orientation is roll, pitch and yaw.** An initial-scene object's
  orientation is always shown and edited as roll, pitch and yaw in degrees; an
  object stored as a quaternion is shown converted and saved as `rpy_deg` when
  edited (same orientation).

- **Projection preparation, process inspection and projection rebuild have
  no time limit (OD-270).** They wait for the execution host's answer
  instead of 10 s; the wait ends early only when the host exits (new code
  `execution_host_exited`; ManyForge stops the application) or ManyForge
  stops (new code `application_stopping`). While a process inspection or a
  rebuild runs, other requests to the host answer
  `runtime_unit_launch_in_progress` (phase `inspecting` or `rebuilding`) at
  once instead of waiting. The Manager logs each projection preparation's
  start and duration. A stop no longer waits behind such an exchange, and a
  second stop request again ends a host that does not stop.
- **A new Apply request no longer waits while an Apply step runs (OD-270).**
  It is refused `apply_busy` with `effect: none`; the replay of an accepted
  Apply is still answered.
- **Apply and Revert keep the live Scene and can reload the initial scene.**
  A Deployment Apply or Revert now keeps the live Scene like a Program Apply
  or Revert (the Composer sends `scene_initialization: "preserve"` instead of
  `"initialize"`); only Load and New initialize it. The Apply and Revert
  confirmations offer **Reload initial scene** (off by default): on every
  Deployment Apply and Revert, and on a Program Apply or Revert when the
  Program's initial scene changes. Once the Apply or Revert is Applied, one
  Reset with that option follows (objects back to their start poses,
  persistent keys to their initial values); its refusal or failure is shown
  as the Reset's own error and the Apply stays. Without it, the live cell
  keeps its current scene. A Deployment Apply that
  removes a component carrying an object fails as before
  (`configuration_preservation_failed`): reload the initial scene first, then
  apply. Objects placed relative to a moved component keep their old cell
  pose until a reload. Revert, like Apply, now Resets a finished, stopped or
  held run first ("Reset run and Revert", in the Program and Deployment
  confirmations). The Composer's Reset-and-Apply
  context (`POST /api/authoring/reset-apply-contexts`) takes an optional
  `action` (`apply`, the default, or `revert`).
- **The UR5 example Scenarios `mock_ur5_pick_and_place`,
  `mock_ur5_pick_and_place_minimal` and `mock_ur5_vision_pose_pick` use the
  UR5e with a Robotiq 2F-85** (`robotiq_2f_85` mounted on the arm's
  `flange`), like `mock_ur5_vision_bin_pick`, instead of RoboPlan's UR5 model
  with its box gripper. Their Programs close and open the Robotiq instead of
  setting a simulated `gripper_closed` value, and attach the part to its
  `tcp` (the mount, the UR5e `tool0`); the pick targets of
  `mock_ur5_pick_and_place` sit 0.14 m above the part so the open fingers
  straddle it. `mock_ur5_pick_and_place_minimal` loads again: its `table`
  now sits in the cell frame, in front of the arm, at the arm's base height.
- **A component fault raises the alarm "Component fault" (OD-247, OD-248).**
  Each component in fault (simulators excepted) now has one alarm,
  `manyforge.runtime.component_fault` (priority high), whose headline is the
  fault's own text. It is Resolved when the fault ends and stays listed until
  acknowledged, with its raised, returned and acknowledged rows in History; a
  changed fault code while the fault lasts resolves the earlier alarm and
  raises a new one. It gates nothing: the cell latch already blocks execution.
  While the cell is latched it is shown inside the latch item. The alarm
  catalogue is now version 2.13.0, so every alarm's condition id changes.
- **"Component stopped responding" clears by itself (OD-245).** The fault
  `component_owner_stalled` stayed until ManyForge was restarted, even when
  the component's internal task finished a moment later, and the component
  offered Recover, which was refused. The fault now lasts only while the task
  has not finished and clears by itself when it does; the cell then runs
  again once every component is healthy and idle, as after any fault. No
  action is offered while it lasts, and its text says to restart ManyForge if
  it does not clear. The log keeps the line written when the stall is
  detected and adds one when it ends.
- **"Component did not stop in time" has its own code (OD-246).** When a part
  of a component does not finish stopping within its time limit (while an
  Apply replaces the component, at shutdown, or when a robot's controller
  binding is replaced, as at Prepare or Reset position), the refusal and the log
  now carry `component_quiesce_not_drained` instead of
  `component_owner_stalled`, which now means only the "Component stopped
  responding" fault.
- **The Manager refuses a run folder that holds a previous run with a typed
  start-up error, before it starts any process (OD-242).** It writes
  `manager_run_folder_holds_previous_run` as its JSON start-up error and
  exits with status 70, as for its other typed start-up refusals; before, it
  wrote a plain message, and Headless, the Program runner and Composer had
  already been started.
- **The assistant's MCP interface refuses a request holding NaN or Infinity
  (OD-232).** The MCP library turned such a number, or one too large to hold
  (such as `1e999`), into an empty value, so ManyForge carried out a request
  the sender had not written. Every tool call other than a Stop that holds
  one is now refused before anything is carried out, with the answer a
  repeated member already gets (HTTP 400, JSON-RPC `-32600`). A Stop is sent
  on as the widest Stop: a `manyforge_kernel` `stop` or `stop_all` with one in
  its body is sent with an empty body, a `manyforge_cycle_control` Stop
  command with one in its payload with an empty payload; both become Stop
  all (a Halt stays a Halt).
- **Stopping the application waits for each step instead of a time limit
  (OD-187).** The Halt is waited for until it is complete (no longer 3 s),
  each of Composer, the Program runner and Headless until it exits (no
  longer killed after 5 s), and the execution host until it exits (it no
  longer ends itself after 10 s). The log shows each step as it starts and
  ends (`application stop: waiting at <step>: …`), and the execution host's
  Halt is shown as its `planned-halt` phase, so the false
  `shutdown stuck … to force: manyforge-operator stop` line during a normal
  Halt is gone. Each `manyforge-operator stop` now sends one SIGTERM to the
  Manager and waits until the application has ended. A second stop request
  (a second `stop`, Ctrl+C or SIGTERM to the operator launcher, or a second
  SIGTERM to the development workbench) ends the processes still running, with `application_stop_forced` and one
  `application_process_killed` line per process; the Manager still writes
  its own records. Driver processes keep their 10 s grace, and the
  container's stop limit stays the outer end.
- **A stop that did not complete is no longer reported as normal
  (OD-187).** When a process was ended by force, the execution host's exit
  was not clean or the Halt did not complete, the Manager ends with status
  74: the operator launcher ends with the new code
  `application_stop_incomplete` (exit status 73), the session's shutdown is
  recorded as unknown, and the next start reports that the shutdown was
  unclean or could not be verified.
- **The development workbench asks the Manager to stop before it stops the
  frontend server**, so the Halt is no longer delayed by up to 10 s.
- **A run end the launcher cannot write has its own code (OD-229).** When
  the operator launcher cannot write the end of a run into the run folder
  (full or failing disk), `start` now ends with `run_end_not_recorded` and
  exit status 71 instead of `operator_configuration_invalid` and 64. The
  next start treats that run as ended uncleanly, as before.
- **The execution host logs how many operation journal records it still has
  to write when its shutdown drain starts** (`manyforge_journal_drain
  queued=N`), before anything outside can end it.
- **Component group actions and Apply steps wait for the component's own
  result (OD-214).** Enable all, Prepare all and the other group actions,
  Recover and enable of several components, and the Disable and Prepare steps
  of a Deployment Apply, Load or New no longer end `unknown` ("didn't finish
  within N s") when the component's lifecycle command time limit passes. Each
  waits for the action's result, which the component itself ends within that
  limit, or for a Stop. A component that reports `component_owner_stalled`
  ends its step at once, `unknown` with that code.
- **A Real xArm Enable no longer waits for a Stop when the controller manager
  doesn't answer (OD-214).** The hardware switch at Enable, and the joint
  state broadcaster switch that follows it, are bounded by the arm's lifecycle
  command time limit: an unanswered one ends the Enable `unknown`.
- **Move until signal and Probe search can take over a moving robot
  (OD-223).** A motion sent to a robot that is still running an earlier
  Program motion is no longer refused when either motion is a Move until
  signal or a Probe search. The running motion's rule keeps watching until the
  robot switches to the new motion, and the new motion's rule watches from the
  moment the new motion is sent. The error `scoped_motion_takeover_unsupported`
  is removed.
- **A Move until signal or Probe search stopped before its rule watches
  reports the real reason (OD-241).** When such a move is stopped or halted
  before it starts, it is handled as any stopped move, with that Stop's
  reason. When its signal is already on at the start, the node fails with
  "Interrupt signal already active" (`motion_interrupt_source_active_at_arm`).
  Before, both ended as "Interrupt result missing"
  (`motion_interrupt_result_missing`), which now remains only for a move that
  ran to its end without a rule result, or a result that does not add up.
- **Less work while a Stop may be waiting (OD-221).** When a component Enable,
  an Apply or the release of a retired engine adds or removes a robot's
  control worker, the threads are started before, and joined after, the
  Kernel's state lock that a Stop needs. The record of an AI-model attempt is
  written to History after the API's core lock is released; a call that races
  shutdown may now be refused with `historian_closed` instead of
  `headless_api_closed`. Diagnostic lines on the motion, replacement and Stop
  paths (for example `manyforge_goal_feedback_first`,
  `manyforge_moving_scale_hold`, `manyforge_loss_cancel`) are printed after
  their locks are released; their text is unchanged.
- **Simulated Mock inputs are delivered like a device's readings (OD-224).**
  A timed change of a node's simulated input and a Mock output's echo are
  recorded a moment after they are made, as a device's reading is; the change
  keeps the time it was made. A node that starts several simulated inputs sets
  them one after the other: a wait on them that begins in that instant, in a
  parallel branch, may see some already set; the node itself still reads all
  its start values at once. These steps no longer hold up the cell's other I/O
  devices. The same applies to the machine I/O banks of an Isaac cell, which
  use Mock I/O.
- **Late Program ticks are counted and logged (OD-213).** A tick of a run
  that ends after its period (10 ms by default) is counted; nothing is stopped
  for it. Each run of consecutive late ticks writes one
  `manyforge_fault code=program_ticks_late` line when it ends (and one every
  1000 late ticks while it lasts) with the count, the longest tick and the
  run's totals; a run writes at most 64 such lines. The work at the end of a
  run (its end with the cell latch, rules back to their start state, the
  ended state in the cycle journal) no longer runs on the thread that starts
  the ticks, so other runs' ticks are not delayed by it.
- **A Program's motions and commands start about 70 ms sooner (OD-234).** The
  execution host reads and verifies the Program and its Program Binding once,
  when it takes on the run, and every start of that run, and every operation
  query of its control nodes, uses that copy (before: it read both files from
  disk and verified them again for every motion, gripper command and query).
  A Program or Binding file altered on disk in the middle of a run is
  therefore no longer noticed at the next motion, only when the next run is
  taken on.
- **A motion starts about 15 ms sooner again (OD-235).** The motion engine
  takes the two Stop plans the Kernel built for the start as they are, instead
  of building both again to compare them; it still checks that each plan
  belongs to this operation and participant. A take-over, whose preparation
  was kept since it was accepted, still has its plans checked before it is
  prepared.
- **The Program runner's full memory clean-up runs at start-up, Load and
  Reset instead of in the middle of a run (OD-234).** After each of these,
  and without a clean-up when a run starts (once its tree is built), it sets
  the long-lived objects aside, so the automatic full clean-up during a run
  no longer scans them; it stopped the whole runner, ticks included, for
  about 150 ms (max 215 ms). Load and Reset answer that much later. When
  automatic full clean-ups still ran since the previous of these points, the
  point writes one `program_runner_heap_settled` warning with their count and
  the longest, the point, the objects set aside and its own pause.
- **A slow output write no longer holds up other requests to the execution
  host.** Output writes (manual and Program) wait for the device on a lane of
  their own, one at a time in arrival order, as before; the Kernel admits each
  one when it starts, not when it arrives. With eight writes already waiting,
  the next is refused with `execution_io_output_lane_busy` (before: it waited).
- **Scene nodes, Set output, a pneumatic gripper's valve writes and Set TCP
  no longer hold up the Program tick (OD-213).** Each node sends its request
  off the tick, one at a time per run in order, and stays running until the
  answer, which it returns on a later tick (about one tick later; a node in
  parallel that reads the Active TCP during Set TCP reads the previous one).
  These are atomic: a Stop that holds the node, or a Halt, before the request
  was sent keeps it unsent until the node runs again; once sent it completes
  and the node returns its real result, never `interrupted`, and the Halt, or
  the end of the run on Reset, waits for that answer.
- **Running motion, gripper and Kernel control nodes no longer ask the
  execution host for their operation's state every tick (OD-213).** The host
  pushes every resident operation's state on its event stream, and the nodes
  read that copy; a node sees its operation end at the first tick after the
  stream delivers it (at most one tick later than before, plus the stream's
  own delay), and a Halt stops a running motion without asking first. While the stream is down the nodes stay
  running until it is back (it is lost only when the host ends, or after a
  runner that takes nothing for 3 s). *Formats:* the event stream is
  `manyforge.execution_event_stream.v4`; its `state.operations` gains
  `resident` (at most 64 resident operations, lowest ids first, each with the
  `read` reply's `operation` without the participants' `progress` and the
  `updated_at_ns`, `control_pending` and `last_control`; `operation` is null
  above 16 KiB) and `resident_total`. The execution command replies are
  `manyforge.execution_command_response.v6`: the `submit`, `reset` and
  `acknowledge_completed` acceptances gain `operations_change`, the table's
  counter after the acceptance.
- **A Program command that meets another one in progress is refused at once
  (OD-144).** The Program runner carries out one command at a time. Until
  now a second command waited for the first and, after 30 s, was reported
  "Program runner not available" but still carried out later. Now it is
  refused at once with `execution_control_ingress_busy`, whose details name
  the command in progress (`command_in_progress`), and it is never carried
  out later; the same command sent again while it runs is answered
  `execution_control_command_in_progress`. The command in progress is
  published in the Program cycle status (`runner_commands.in_progress`, see
  the next entry). Every panel disables Execute, Reset, Run/Resume,
  Retry/Continue, value writes and stored-value edits meanwhile, with the
  reason "Not available while another Program command is running."; Stop,
  Stop all and Halt never pass through the runner and stay available. The
  automatic scene load after an Apply retries a busy runner within its 30 s,
  as it does while the runner is not ready. *Integrators:* treat
  `execution_control_ingress_busy` as "nothing was done; send again when
  `runner_commands.in_progress` is null".
- **Program commands are answered when they are accepted, and the cycle
  status shows each command's step and how it ended (OD-214).** Execute,
  a group's Run and Resume, the handover before manual moves, Execute after
  Halt, recovery actions and manual results, Program-value writes,
  stored-value edits and Reset are answered as soon as the Program runner has
  recorded them, with `manyforge.program_command_reply.v1`
  (`{schema_version, command}`); before, they were answered only when done.
  This reply replaces `manyforge.program_runtime_control_result.v2`,
  `manyforge.cycle_recovery_result.v3`,
  `manyforge.program_cycle_reset_result.v1`,
  `manyforge.persistent_key_edit.v2` and the run returned by Execute,
  Run/Resume, the handover and Execute after Halt; the run, the controls,
  the recovery offer and the stored values are read from the status as
  before. A refusal is answered at once with its code, as before. The
  Program cycle status replaces `command_in_progress` with
  `runner_commands`: the command holding the runner and the last 8 that
  ended, each with its kind, request key, step (for example `admitting`,
  `claiming`, `applying`, or Reset's phase), acceptance time and end
  (`succeeded`, `refused`, `failed` or `unknown`, with its error code); the
  control-state stream is `manyforge.program_control_state.v4`. A request
  sent again with the same key gets its command's row back and is not carried
  out twice. A refusal given at once because another command is in progress
  is not a row. Scene preparation after an Apply still answers when done.
  *Integrators:* read a command's outcome from its row
  (`runner_commands`, by request key), not from the reply.
- **The Composer shows a running command's step and, when it does not
  succeed, its reason (OD-214).** While a Program command runs, the reason
  on the buttons it holds back names the command and its step (for example
  "Execute, claiming the cell"); an Apply, Revert, Load or New names its step
  the same way. When a command you sent ends other than succeeded, the
  error-catalogue text of its code is shown where its refusal was shown
  before. The Composer no longer gives up on a command's reply after 5 s and
  shows "Command delivery is unconfirmed" only when the reply is lost. For
  the commands it sends over HTTP (Execute, Run, Resume, Execute after Halt,
  recovery actions, manual moves, Apply, Revert, Load, New and stored-value
  edits), sending the same command again after a lost reply reuses its
  request key, so the owner answers with that command's row and does not
  carry it out twice. Commands sent on the control WebSocket (Reset,
  Program-value writes, manual results) are named by the control session and
  their place in that session, so an answer is kept only within one session:
  after a reconnect, sending such a command again is a new command.
- **Plan pick sequence and Plan place sequence no longer hold up the
  Program tick (OD-213).** The node asks the execution host to plan and is
  RUNNING while it plans; its parallel siblings keep running. It follows its
  plan on the host's event stream and reads the result once, when it is
  ready. The failure after 30 s with no answer is gone. A Halt, a Stop that
  holds the node, the end of its branch and Forced Program End cancel this
  plan only (the node reports `interrupted`); after Execute or Run the node
  plans again from the start. A Stop no longer cancels the plans of other
  robots, and a planning failure reaches the node with the host's own code
  (for example `pick_planning_cancelled` after a Reset).
- **Pick and place planning is a job that is answered at once (OD-213).**
  `POST /api/execution/pick-place/plans/pick` and `…/plans/place` answer as
  soon as the plan is accepted, with `planning_job` (`job_id` = the request's
  `sequence_id`, `state` `accepted`), instead of waiting for the result. The
  job is read with `GET /api/execution/pick-place/plans/{job_id}`; its
  `state` is `accepted`, `planning`, `done` (with `planning_result`, as the
  plan reply carried before), `failed` (with the typed `error`) or
  `cancelled`. `POST …/plans/{job_id}/cancel` cancels this plan only, and
  `POST …/plans/{job_id}/release` drops a finished job; the host keeps at
  most eight jobs (`execution_planning_jobs_busy` beyond that,
  `planning_job_exists` for a name still held, `planning_job_unfinished` for
  a release before the end) and drops a Program run's jobs at its Program
  Reset. A Stop no longer cancels plans; a Reset still does.
  *Integrators:* read the job until `done`, `failed` or `cancelled`, then
  release it.
- **ManyForge keeps its own files within disk limits and does not start on a
  nearly full disk, in both launchers.** At each start, before anything is
  written, the launcher prunes previous runs and older History files (five
  runs, 2 GiB) and, while ManyForge's files take more than 10 % of the disk or
  less than 10 GB is free, deletes more of them, oldest first; the newest
  run, the reported run and the current files are never deleted. While
  running, it repeats this every 60 seconds, only when a limit is exceeded.
  Each deletion is one log line. Below 10 GB free (or with no free inodes) on
  the disk that holds the records, `start` is refused with
  `disk_space_below_floor`, naming the disk and how much to free, instead of
  failing later with the system's "No space left on device". The limits are
  built in and can be changed in the site settings file, now
  `manyforge.site.v2`, by three optional members: `disk_max_percent` (default
  10), `disk_always_free_bytes` (default 10000000000) and
  `disk_machine_minimum_bytes` (default 0, not determined yet; it is added to
  the free space a start needs). `manyforge.site.v1` files are still accepted
  and use the defaults; a file with a disk member must say
  `manyforge.site.v2`. `manyforge-operator start` has a new optional
  `--site-settings PATH` (standard profile), read only for these limits.
  The Headless API accepts `--disk-free-floor-bytes` and
  `--disk-free-margin-bytes`, which both launchers pass.
  *Developers:* `MANYFORGE_LOG_EVIDENCE_MAX_DISK_PERCENT` and
  `MANYFORGE_LOG_EVIDENCE_MIN_FREE_BYTES` are removed; put their values in the
  site settings file as `disk_max_percent` and `disk_always_free_bytes`.
  `start` refuses to run while either variable is set.
- **Collision checks: one thread per robot, and a stop when they do not run.**
  Each moving robot's collision checks now run on a thread of their own, so
  one robot's checks cannot delay another's. A run of late checks writes one
  log line. If no check has verified a recent robot position within two check
  periods, the robot's own motion worker stops it: with the alarm "Collision
  checks stopped" (`runtime_monitor_checks_stopped`) when the checks did not
  run, or with "Robot position feedback lost" when the checks ran but the
  position was old. Both need Recover.
- **Operation stops while the disk cannot keep ManyForge's records.** The
  Headless API checks, on a thread of its own, the free space (and free
  inodes) on the disks that hold the records once a second, and the Kernel
  journal's and History's queues. When free space falls below the floor, or
  either queue is full, it sends one Halt (the run status shows "Halted: the
  disk cannot keep ManyForge's records.") and raises the alarm
  `manyforge.runtime.disk_cannot_keep_records` (priority high, no
  acknowledgement). While it is active, Run, Resume, Program motion,
  take-over and retry are refused as for any alarm interlock, and manual
  moves and a Reset's move back to the start position are refused too
  (`manual_move_disk_cannot_keep_records`,
  `reset_position_disk_cannot_keep_records`); Stop, Execute after Halt,
  Reset without the move back, Apply, Load, New, component actions, Save and
  History stay available. A Reset that asks for the move back is refused
  before it starts (`program_cycle_reset_position_unavailable` with that
  reason), and the Reset dialog shows the reason at the move-back option.
  The alarm clears by itself once free space is back
  at the floor plus 1 GB and both queues have been written empty; nothing
  moves by itself (press Execute, then Resume or Run). A single record that
  cannot be written still only drops and counts it. The alarm catalogue is
  2.12.0; the "Operation records are being lost" and "History is missing
  records" alarms no longer say "Motion is not affected.". *Formats:* the
  event stream's (`manyforge.execution_event_stream.v4`) and the `events`
  reply's `runtime_faults.operation_journal_write_fault` gains `queue_full`;
  the motion-attention projection
  (`manyforge.operator_alarm_motion_attention.v4`) gains
  `disk_cannot_keep_records`.
- **Operation alarms and panel updates come from the Kernel's memory, not
  from the journal file.** The execution host's event stream is now format
  `manyforge.execution_event_stream.v4`: its `state` message carries
  `operations` (a change counter and the operations currently in a fault
  state, with the facts their alarms show), and each round sends `state`
  before the journal rows. The motion-fault, collision and lost-feedback
  alarms are raised and cleared from that table: a slow or stalled journal
  disk no longer delays or loses them. Journal rows remain History records.
  An alarm now also clears when the operator's Recover or Retry closes its
  operation (before, it stayed until the end of the start). A manual move
  that fails before it is sent shows no alarm, only its refusal and its
  History entry. A fault that starts and ends between two stream messages
  is in History but not shown as an alarm. An alarm's error message is cut
  at 256 bytes and ends in "…"; History keeps the whole text. If more than
  32 operations are faulted at once, or an identity is longer than 128
  bytes, the operation alarm source reads as not fully reconciled.
  *Integrators:* stream clients must accept v3 (the `events` command reply
  is unchanged).
- **Recover starts a component's exited process again, for every kind.**
  When the process behind a component ends by itself, Prepare or Recover
  starts it again, as they already did for an xArm: Mock and Isaac robots
  and grippers (their launch) and I/O devices (simulated PLC, pose source,
  AI model). The component then waits for the new process within its
  lifecycle command time and ends prepared; nothing is enabled and nothing
  moves. The fault of an exited I/O device now offers Recover, so "Recover
  and enable" works for it too. A failed restart fails the action with
  `component_driver_restart_failed`; the fault stays and the operator can
  try again. No Load is needed any more.
- **A failed ROS connection inside ManyForge has its own fault code.** A
  component whose in-host ROS transport failed now shows
  `fault_ros_transport_failed` ("ROS connection failed") instead of
  `fault_process_exited`; Recover rebuilds it, as before. *Integrators:*
  handle the new fault code where you matched `fault_process_exited`.
- **The execution host reads neither the Manager's Apply journal nor, after
  start-up, its publication store.** It acts on what the Manager's requests
  carry: an Apply's claim release states whether that Apply succeeded, its
  claim acquire names the candidate bundle, and liveness and fence requests
  carry the current publication. The host learns that an Apply succeeded when
  that release arrives: until then the Program-value context answers
  `bb_source_not_ready` and the empty-cell global rules wait. Refusals and
  stalls caused by reading those files are gone.
- **Shutdown stops the cell first.** Every end of the application (a stop
  command, a stop signal or a process exit) runs one sequence: the Manager
  asks the Kernel for a Halt and waits up to 3 s for it to settle, then
  stops Composer, the Program runner and Headless in that order (each
  SIGTERM, up to 5 s, then SIGKILL), then shuts the execution host down, whose
  first step is again a Halt with up to 3 s to settle. A Stop that
  cannot be made or does not settle in time is logged and shutdown
  continues. The workbench's `docker stop` allows 30 s (was 15 s).
- **Headless never refuses a Stop as busy.** Stops wait only behind other
  Stops (two at a time) and are never answered
  `execution_control_ingress_busy`; ordinary commands keep their busy answer.
- **A Stop partly under running Stops stops only what is not stopping yet.**
  It gets its own Stop event; robots already stopping are covered without
  restarting their Stop, and the event completes when everything it covers
  has stopped. A repeated Stop (same request key, same content) returns the
  first answer. Before, an overlapping Stop was widened to a stop of the
  whole cell (`overlapping_stop_transactions`).
- **The execution host is no longer stopped when it answers the Manager
  late.** The late request answers `execution_host_late` (outcome unknown),
  further requests to the host answer the same at once, and the Manager logs
  `execution_host_late_reply` once and sends one Halt. The host counts as
  faulted only while its reply is outstanding and stops counting when the
  reply arrives; the Halt stays until Execute or Reset. Before, the host was
  stopped, which ended the application. New error code:
  `execution_host_late`.
- **The application no longer ends by itself when the Manager's management
  channel to a running execution host fails.** The host is handled as a late
  host that never answers: requests answer `execution_host_late`, the Manager
  logs `execution_host_late_reply` once and sends one Halt, and the host counts
  as faulted until the operator stops ManyForge and starts it again. Before,
  the Manager stopped the host, which ended the application. A host that has
  exited still ends the application.
- **A malformed Manager liveness message no longer ends the execution
  host.** The host refuses it, logs `Manager liveness packet refused: …`, and
  latches the Kernel's internal fault (`manager_liveness_packet_refused`),
  which stops the cell and blocks motion until the application is started
  again.
- **A request that meets the execution host still launching or preparing is
  answered at once** with `runtime_unit_launch_in_progress` (HTTP 503)
  instead of waiting; retry it.
- **The operation journal names its start once** and stores operation and
  Stop event numbers as integers and request keys as 16 bytes.
- **Execution actions no longer wait for the operation journal.** Every
  action takes effect in memory; one writer thread writes its records
  afterwards, in the order of the actions, with SQLite `synchronous=NORMAL`.
  A failed write no longer refuses or undoes an action; the record is counted
  as not written. A row's `occurred_at_wall` is the
  time of its action, not of the write. Rows reach the event stream and
  History once written. A robot's Active TCP change (Set TCP, the TCP step at
  a motion's start, Reset) goes through the same writer, so a slow disk no
  longer delays motion starts on that robot and History shows the change
  after the rows of earlier actions.
- **The alarm `manyforge.runtime.operation_journal_write_fault` is now
  "Operation records are being lost"** (low priority, no acknowledgement):
  raised while the execution host drops records (full queue or failed
  write), cleared by itself once the journal writes again, never blocks
  motion; the journal marks such a start in `journal_metadata.records_lost`.
- **Cell-admission claims are two journal rows,** `cell_claim_acquired` and
  `cell_claim_released`; the four claim tables are gone. A claim takes effect
  in memory and is never refused or left held by a journal write, so
  `persistence_outcome_unknown` is removed. The previous-run report counts a
  claim acquired without a release as unresolved.
- **The previous-run report lists unfinished operations.** The next start's
  report on the previous run (`manyforge.workbench_previous_session.v2`)
  lists up to 64 unfinished operation numbers of the previous run, each
  marked `host_replaced`, and their count.

- **The execution Kernel answers every Program Stop.** A group Stop, Stop all
  or Halt goes from Headless to the Kernel and never through the Program
  runner, which learns of it from the Kernel's state. The Kernel never
  refuses it:
  - A repeated Stop with the same request key and content returns the first
    answer; the same key with different content becomes Stop all (a Halt
    stays a Halt; `widen_reason: key_conflict`).
  - A group Stop naming a run that ended in this application start has no
    effect; one naming a run number higher than any run started becomes
    Stop all (`unknown_target`).
  - A group Stop whose path and body name different runs is Stop all
    instead of `program_run_path_invalid`.
- **Composer keeps its control connection open instead of closing it on a
  fault, so Stop keeps working.** Each case is recorded so its cause can be
  found:
  - A WebSocket command whose delivery the owner did not confirm within 1 s
    no longer closes the connection. Commands other than Stop stay disabled
    until the owner's watermark reaches it; the `command_delivery_unconfirmed`
    incident now names the command kind and sequence
    (`command_watermark_missing:<kind>:<sequence>`) and is resolved when the
    watermark arrives.
  - A Run, start or recovery command whose HTTP outcome is unknown (server
    error, network error or timeout) no longer closes the connection.
    Commands other than Stop stay disabled until the owner state shows a
    newer control revision or cycle generation; the incident names the kind
    and outcome (`http_response_unconfirmed:<kind>:http_<status>|network|timeout`).
  - An invalid control message while a Stop could still matter (a Program
    cycle starting, running, stopping, resetting or needing recovery; a run
    held, halted or halting; an unfinished manual operation) no longer closes
    the connection: Composer shows "Control state is invalid; commands other
    than Stop are disabled. Reconnect when motion has stopped.", stops
    applying owner state and still sends Stop. With nothing active it
    reconnects as before. Either way it is recorded as the new client
    incident `control_projection_invalid`
    (`parse_error|non_monotonic|invalid_projection:<message kind>:<sequence>`),
    resolved once a later session applies valid state.
  - These latches on an open connection are titled "Control commands
    unavailable" (with Reconnect); "Control connection unavailable" stays
    for a connection that is down.
  - A control connection that stays stale is replaced after 30 s, and only
    while no run or operation is active (recorded as `control_disconnected`,
    `control_stale_reconnect`).
  - The Composer backend relay waits up to 30 s (was 0.25 s) for the browser
    to take a frame. A send slower than 0.25 s, or one that times out, is
    logged at a bounded rate with the control session id, message kind and
    wait time.
- **A group Stop while the Program run is already stopping or halting is
  accepted instead of being refused.** It was answered with
  `program_motion_stop_invalid` ("motion Stop requires a current running
  Program"); now the Kernel accepts it like any Stop, and what is already
  stopping is not restarted.
- **Composer never refuses a Stop.** Group Stop, Halt and Stop operation are
  available whenever there is a run or an operation to stop. Nothing else in
  progress disables them: not a starting Execute, another pending command
  (such as an unconfirmed speed change), an earlier Stop or Reset still
  pending, a runner projection not yet loaded, or a control connection that
  is delayed, stale or synchronizing. A group Stop stays unavailable only for
  a group that is already held (or not started), also while a Stop or Halt
  ends the run; the **All groups** row's Stop (Stop all) and Halt are always
  available, also with no Program loaded. A repeated press sends
  another Stop, which covers what is still stopping without restarting it or
  has no effect; the button keeps its Stopping/Halting label. A Stop is sent
  whenever the control WebSocket is open, also while the heartbeat is late
  or after an earlier command's delivery was unconfirmed; other commands keep
  their gates, and the connection and control-state notices (delayed,
  stale, synchronizing, unavailable, delivery unconfirmed) now say
  "commands other than Stop are disabled". An `operation_stop`, `stop_all` or `halt` is sent without
  `expected_execution_owner_epoch` when Composer does not know the owner
  epoch. Robot Control keeps an active operation's panel, and its Stop,
  while the connection is not current.
- **Halt sends Stop all at once and asks only about halting the Program.**
  With *Confirm before halting the Program* on (the default), Halt sends Stop
  all without waiting and opens the confirmation at once; a refused Stop all
  does not cancel the Halt, and the confirmation says "Stop all not
  confirmed". It says "Motion is stopped." only once all motion has stopped,
  and shows "Waiting for all motion groups to stop…" until then. *Halt
  Program* is always clickable and sends the Halt; Cancel leaves every group
  stopped, ready for Resume. With that setting off, Halt sends Stop all, then
  the Halt, at once.
- **The Manager no longer holds leases on the Program Runner and the
  Kernel.** The Manager still reports its identity, the Runner's cycle status
  and cell readiness every 250 ms, but a report never expires. A Manager or
  Program Runner that is late or stalled (1 s, 10 s or longer) no longer
  stops the cell, inhibits motion, stops the running Program or makes the
  execution host exit with code 70. Motion already admitted stays under Kernel
  supervision. A failed report closes cell readiness (no Stop) and the next
  successful one reopens it. A Program Runner that stops answering during an
  active Program cycle still raises `program_runner_loss`, and the Kernel now
  stops that Program run's motion each time a loss begins (a program-run Stop
  with source `kernel.program_runner_loss` and reason `program_runner_loss`;
  not Stop all, nothing else in the cell is stopped; the run stays frozen
  until Execute or Reset). The loss is no longer kept for the whole
  application start: the "Program Runner lost during a run" alarm clears by
  itself when the Runner answers again, and its text no longer asks for a
  restart. Before, the expiring lease stopped the whole cell.
  Removed with it:
  - the Runner's 1 s expiry, its Manager-loss Stop (`manager_presence_expired`),
    its durable Manager-loss record and the `/api/program/liveness/status`
    route; a new Manager epoch is accepted and reconciled directly instead of
    waiting for the old one to expire;
  - the Kernel receiver's expiry timer and its scheduler thread, the
    `manager_epoch_expired` rejection, the Manager's retry gate, the host's
    wait for that expiry on a closed Manager channel, and the refusal of
    heartbeats after a stuck drain (`drain_stuck` still prints and waits);
  - the development workbench's wait for the Runner to see the Manager
    expire on shutdown.

  Formats: the retained `manager_liveness_policy` drops
  `expiry_timeout_ns` (runtime manifest
  `manyforge.ros2_control_execution_runtime_manifest.v2`), the private
  Manager-to-host liveness packets move to `v2`, and the Program-cycle journal
  moves to schema 10. Bundles and journals of earlier versions are refused.
  Error codes no longer produced and removed from the catalog:
  `manager_epoch_expired`, `manager_presence_expired`, `receiver_destroyed`,
  `watchdog_schedule_failed`.
- **Bounded work on the time-critical paths.** The request replay ledgers,
  Program-run fences and operation history are indexed by id in slots
  allocated once, so a Stop finds, adds and evicts entries without scanning or
  moving them and without new allocations. The global I/O rule projection
  queue is a fixed ring; scheduler lanes pick the next task without a scan;
  the gripper late-cancel prune no longer runs under the gripper authority a
  Stop takes; repeated Stop-path collision facts are recorded once. The
  runtime-scale replay ledger is now capped at the request replay capacity:
  the oldest entry is evicted, and a repeat of an evicted request id counts as
  a new request. An I/O signal's `ingress_depth` above 1024 is refused at
  Apply (`io_signal_configuration_invalid`), and Composer accepts at most 1024.
- **Speed changes no longer wait for planning, Submit or the disk.** The
  execution host runs speed changes on their own lane (thread
  `mf_scale_lane`), fed from both its sockets; the Headless sends them on the
  control socket beside Stop. The new value reaches every arm before the
  reply, and so does the moving operation's own speed step (its new speed
  generation); when that operation is busy with another change at that
  moment, the step runs at the next status poll, about 5 ms later, and if
  several values arrive meanwhile only the latest is recorded. The step never
  waits for the disk: its `runtime_scale_accepted` record is written
  afterwards, in the order of the actions, and a record that cannot be written
  is counted and raises the journal alarm while the status still advances. The
  reply no longer names the active operation (`active_operation_id`,
  `active_operation_revision`, `active_operation_scale_generation` are
  `null`); the runtime-scale status and an exact replay still show them. An
  operation's per-operation scale status stays `pending` until the next status
  poll. With eight speed changes already waiting, the next is refused with
  `execution_runtime_scale_lane_busy` (before: `execution_submission_lane_busy`).
- **Stops no longer wait behind component actions, the component model or
  alarm attention.** The execution host serves component actions (also
  Recover and enable), their recovery preflight, the component model and the
  operator-alarm attention heartbeat on a new socket,
  `execution-component.sock` in its run root, with its own thread. The control
  socket keeps every Stop form, the Stop status, carried-object-lost and speed
  changes, and refuses the moved commands with
  `execution_control_command_unsupported`; the new socket refuses every other
  command with `execution_component_command_unsupported`. Packet and reply
  formats are unchanged.
- **Stops no longer wait behind I/O deliveries when a motion's interrupt
  rules are added or removed.** Adding and removing the rule of a Move until
  signal or Probe search no longer waits for the I/O devices' deliveries
  (as for global rules already), so a Stop, a motion
  start and the collision checks no longer wait behind a Mock simulation step
  or an Apply installing I/O devices. In Mock, a rule added at the very moment
  a simulation step changes several signals may see that step half done, as
  on real hardware, where signals never change together. The execution host
  starts all of its scheduler threads when it starts, in both resource
  profiles, so no request waits for a thread to be created on first use.
- **Interrupt rule states no longer wait for the disk.** The Program runner's
  `interrupt_state.<rule_id>` keys and the Global rules panel take each
  global rule's state from the execution host's memory, as its event stream
  reports it, and no longer wait until the rule's History row is written;
  during a slow disk `notify` keys no longer go without a value and `stop`
  keys no longer lag. A trip that starts and ends between two of these
  reports no longer shows `tripped` in the key or the panel, for `stop` and
  `notify` rules alike; the Stop and the hold of a `stop` rule are
  unchanged, and History still lists every transition. The ready rule-state
  baseline of the event stream and the `events` reply no longer has
  `through_sequence` (inside `manyforge.execution_event_stream.v2` and
  `manyforge.execution_event_page.v3`).
- **The Manager's status files no longer wait for the disk.** The Manager
  still rewrites `manager-source.json`, `logical-scene-source.json` and
  `scene-runtime-source.json` every 500 ms (its heartbeat to the Headless),
  and `viewer-pose-sessions.json` on change, but no longer syncs them or their
  directory. Readers still see one complete record. After a power loss a torn
  status file is replaced at start-up instead of stopping the Manager from
  starting.
- **The Program runner's cycle journal is written with SQLite
  `synchronous=NORMAL`,** like the operation journal and the Manager's
  stores (before: `FULL`). After a power loss the next start's report may
  miss the journal's last records.
- **A Stop naming another application start, or none, now says why it was
  widened.** An operation or Program group Stop sent over HTTP with a missing
  or other `application_start_id` still acts as Stop all and is never
  refused; its Stop record and reply now carry `widen_reason` `stale_start`,
  `requested` `operation` and the reason `recognized_stop_start_not_current`
  (before: `none`, `stop_all` and no reason), and the reply's `command` is
  `recognized_stop` (before: `stop_all`). The record keeps the caller's
  source label. A Stop whose body is invalid is answered with `command`
  `recognized_stop` (before: `malformed_stop`); when the Program group Stop's
  path and body name different runs, or a Stop on the control WebSocket or
  `/api/control/commands` has an invalid payload, its record now carries the
  caller's source label instead of `recognized_stop_ingress`.

### Removed

- **The `ur5_roboplan` model (`universal_robots.ur5_roboplan`, OD-283).** No
  example used it; UR5 cells use `universal_robots.ur5e` with a gripper
  component. What only it used went too: the installed-package model-resource
  fallback in the deployment build, the `remove_unused_xacro_namespace` URDF
  projection, the CI preparation of `.docker/roboplan` for generated checks
  and Composer tests, and its old behavior test fixture.
- **The Apply time bound (OD-270).** An Apply is no longer ended
  `apply_timed_out` after the sum of its components' budgets; an Apply step
  ends only on its own events, and the code `apply_timed_out` is gone.
- **The public Program Stop.** `POST /api/program/runs/{program_run_id}/stop`
  and the control command `program_stop` are gone. The Program runner's own
  Stop routes (`/stop`, `/motion-stop`, `/full-stop` and
  `/discard-full-stop` on its private port) are gone too.
- **The Program Full Stop.** `POST /api/program/runs/{program_run_id}/full-stop`
  and the control command `program_full_stop` are gone; Halt
  (`POST /api/execution/halt`, control kind `halt`) replaces them.
- **The Manager's component-process Stop.** The Manager commands
  `stop_component_process`, `component_process_stop_result` and
  `component_process_stop_offer` are gone; they served the forced "close
  Scenario" flow removed in 0.2.0.
- **Takeover and reconciliation of a previous start.** Owner epochs, the
  Manager-epoch replacement, in-place execution host replacement,
  cell-admission claim reconciliation, orphan-goal reconciliation and
  `POST /api/program/authoring-transactions/reconcile-owner` are removed:
  nothing from a previous run is adopted.
- **The POSIX reference runtime adapter** and its reference product
  verifier; the Manager runs only the execution host.
- **The execution host's `--manyforge-managed-manager-state-root` argument.**
  The host no longer opens the Manager's publication store at start-up; it
  never built anything from it, because every start publishes the empty cell.
  The Manager no longer passes the argument, and the host refuses it as an
  unknown option. The Manager keeps its own `--manager-state-root`.
  *Integrators:* nothing changes when ManyForge is started through a launcher
  or the Manager.
- **Stop fingerprints.** Stop replies and records no longer carry
  `requested_selector_digest` or `captured_scope_digest`.
- **`effect_pending` and request tombstones** in the operation journal and
  its events.
- **In-place History upgrades.** A History file is never converted to a newer
  format.
- **Unused operation-journal tables and self-checks.** The operation journal
  no longer writes the tables `operation_participants`,
  `operation_resources` and `operation_error_details` (the operation state is
  in each record's payload), no longer re-reads and compares its snapshot on
  every write, and its Stop event index is no longer unique.
- **The Kernel's Apply-fence file.** The execution host keeps its Apply
  fences in memory; `execution-state/kernel-apply-fence.sqlite3` and its
  `.lock` are no longer created, and taking an Apply fence no longer waits
  for the disk.
- **The Apply fence re-read.** The Manager no longer asks the execution host
  to re-read a fence it has just taken, so an Apply makes one exchange fewer;
  the `MFNKAFR` Rehydrate request and the fence state "release pending" are
  gone. Removed from the catalog: `configuration_fence_rehydration_failed`.
  *Integrators:* drop handling for the removed code.
- `--operator-readiness-timeout` of `scripts/dev-workbench.py`; the operator
  launcher's `standard` start no longer has a readiness timeout
  (`--readiness-timeout-s` is still accepted and used only by `isaac`).
- **Error codes that nothing produces.** Removed from the catalog:
  `component_lifecycle_recovery_catalog_unavailable`,
  `component_lifecycle_recovery_observation_unavailable`,
  `component_lifecycle_recovery_port_unavailable`, `manifest_set_mismatch`
  and `original_owner_required`. *Integrators:* drop handling for the removed
  codes.
- **Scene recovery when the Scene store is opened.** Every start creates a
  new, empty Scene store, so there is nothing to recover. Removed from the
  catalog: `scene_projection_recovery_uncertain`,
  `scene_projection_rejected_rollback_proven` and
  `scene_request_recovered_before_projection`. *Integrators:* drop handling
  for the removed codes.
- **Identity, sequence and digest checks on the private liveness messages.**
  The Manager, the execution host and the Program runner no longer compare
  the cell, Manager, receiver, start identity, policy, bundle or required-set
  identity of a liveness message with values of the same start, recompute
  its digests, or require strictly increasing heartbeat numbers; within one
  start they cannot differ. Removed from the catalog:
  `heartbeat_identity_mismatch`, `heartbeat_sequence_not_strict`,
  `manager_liveness_identity_mismatch` and `receiver_clock_out_of_range`.
  *Integrators:* drop handling for the removed codes.
- **The History sequence-gap mark.** Operation events in History
  (`manyforge.operation_history_event.v2`) no longer carry
  `native_sequence_gap`; it is gone from the History detail and from History
  exports.
- **`recovery_ready` in the History health.** `GET
  /api/execution/history/health` (`manyforge.async_historian_health.v2`) no
  longer carries `recovery_ready`, which was always false.
- **`storage_fault` in the stored values.** The stored values
  (`manyforge.persistent_key_values.v2`) no longer carry `storage_fault`,
  which was always null, and the Stored values panel no longer has its alert.
- **More error codes that nothing produces.** Removed from the catalog:
  `control_file_watch_unavailable`, `headless_source_snapshot_unavailable`,
  `logical_scene_source_unavailable` and `scene_runtime_source_unavailable`.
  *Integrators:* drop handling for the removed codes.
- **Error codes of the removed component lifecycle machinery and other codes
  that nothing produces.** Removed from the catalog: the
  `component_lifecycle_*` codes `admission_invalid`,
  `admission_invariant_failed`, `catalog_mismatch`, `configuration_stale`,
  `current_snapshot_mismatch`, `effect_uncertain`, `facts_stale`,
  `partial_effect_observed`, `primitive_known_no_effect`, `scope_unresolved`,
  `state_reconstructed` and `transition_missing`; the `native_candidate_*`
  codes `catalog_mismatch`, `component_missing`, `controller_binding_mismatch`,
  `controller_generation_mismatch`, `controller_membership_incomplete`,
  `controller_mismatch`, `domain_mismatch`, `generation_stale`,
  `membership_incomplete`, `owner_unavailable`, `plan_changed`,
  `plan_mismatch`, `provider_mismatch`, `registration_mismatch` and
  `source_failed`; `snapshot_digest_mismatch`,
  `snapshot_identity_not_canonical`, `snapshot_shape_invalid`,
  `component_stream_acknowledgement_required`,
  `stopped_node_state_unreadable` and `log_path_invalid`.
  *Integrators:* drop handling for the removed codes.

### Fixed

- **No false component fault while the xArm arms (OD-310).** Enabling or
  recovering a real xArm pauses its joint-state and I/O streams for about
  3 s while the vendor driver restarts its controllers; the arm and its I/O
  showed that as a component fault ("stream stopped arriving") that latched
  the cell and needed an acknowledgement. While the adapter's own arming
  runs, the arm shows *Arming the robot* instead; its Enable still verifies
  the streams before it succeeds and fails typed if they never return.
- **No fault from components leaving the cell (OD-311).** Apply and Load now
  take leaving components out of the cell before stopping their processes,
  so their streams stopping (for example the xArm I/O stream) is no longer a
  fault of a cell member.
- **Changed recovery causes ask for confirmation again (OD-312).** When the
  causes to recover change after the operator confirmed them (an e-stop C2
  appearing), the activation now says *The causes changed since you
  confirmed them. Review them and confirm again* and offers the confirmation
  (error code `component_recovery_confirmation_changed`), instead of failing
  with "Cell state changed".
- **A loaded machine no longer refuses motion through the alarm-attention
  heartbeat (OD-307).** Headless gave each publication of the operator-alarm
  motion attention to the Kernel 200 ms; six slower answers in a row (about
  2 s) let the Kernel's copy age out, so Run, Resume and recovery were refused
  although the Kernel was answering. A publication now waits for the Kernel's
  answer, the Kernel's closed connection or Headless's own shutdown; the
  Kernel's 2 s freshness bound alone judges whether its copy is current.
  Publication counts no longer have an `abandoned` outcome.
- **Program forensic records are no longer dropped on a slow machine
  (OD-309).** The Program Runner's delivery of diagnostic captures to the
  Historian gave each request 0.25 s and its shutdown 2 s; a slower Headless
  answer counted the record as lost, and a slower shutdown raised a shutdown
  error. Only the connection is now limited (as for runner commands); the
  shutdown waits for the one delivery in flight.
- **A slow execution host no longer fails the Manager's Stops (OD-306).**
  The Manager's exchanges on the host's Stop control socket (the
  carried-object-lost signal and Stop, and the Halt at the application stop
  with its reads of the Stop's state) gave up after 250 ms. A slow but live
  host then made the carried-object-lost Stop read `carrier_stop_unavailable`
  and the planned application stop end incomplete (exit 74, an unclean
  shutdown reported at the next start). These exchanges now have no time
  limit, like the Program Runner exchange: each ends on the host's reply, on
  the host closing or refusing the connection, or on the application stop
  (typed `application_stopping`; at the application stop's Halt, a second
  stop request).
- **The execution host is stopped without holding the adapter's lock
  (OD-308).** Stopping the host held the adapter's main lock for the whole
  drain (seconds, no limit), so every other host call waited behind it. The
  unit is now marked busy `stopping` while the host drains with no lock held:
  other calls get the typed `runtime_unit_launch_in_progress` answer (phase
  `stopping`) at once, and the host still counts as present until it is gone.
- **The scene status never goes back to an older value (OD-302).** The
  Manager publishes the `logical_scene` and `scene_runtime` status sections
  from the command strand and from the Apply lane. A slower publication could
  replace a newer value with the older one it had read, until the next
  publication; a wait for a Scene commit could then end and a later read
  still show the revision before it. Each value now carries the number of
  the read it was built from, and the status board keeps only a value whose
  number is above the one it holds, decided in the same lock-free swap.
- **A held Manager is noticed after a reconnect (OD-304).** When the status
  link dropped and came back while the Manager's loop was held, Headless
  showed the Manager as answering until its silence bound and then changed
  to "not answering" without waking the control state and viewer; on-demand
  reads were right, pushed updates waited for the next frame. The readers are
  now woken when that silence ends.
- **Move To "Face down" faces down.** It set the TCP to the base frame's
  orientation, so the tool's approach axis (Z) pointed up. It is now a half
  turn about X: Z points straight down.
- **An omitted `target_reference` or `offset_reference` says what it means.**
  It showed as "frame · Select a frame", as if a required choice were
  missing; it now reads *Not set*, or *Same as target_reference* for
  `offset_reference` (the catalog default).
- **Hovering Move preview highlights `target_reference` and
  `offset_reference`** when the copy changes or clears them, as one field.

- **The Composer works from another machine over plain http.** Opened by IP
  address (for example `http://<host>:3000`), the Program reference check
  never ran: it hashed the Draft with the browser's `crypto.subtle`, which
  browsers only offer on HTTPS or `localhost`. The Properties panel then
  showed every target and frame as unavailable and offered no TCPs. The
  check no longer hashes in the browser; a stale answer is still discarded
  by the Draft's generation.
- **An unchecked reference is no longer an invalid edit.** Until the Draft
  is validated, a target, frame, body or signal field shows its value as
  *not checked yet* instead of reporting an invalid local edit that
  **Discard edit** could not clear.

- **A stop request ends every wait on the execution host at once.** A
  SIGTERM or SIGINT to the Manager (`compose-dev.sh stop`, `docker stop`)
  while the host was still starting, or while a Scene commit or an Apply's
  acceptance waited on a busy host, was not seen until the host answered, so
  the orderly stop (planned Halt, process stop, records written) did not
  start and Docker killed the Manager after its grace period. Such a wait now
  ends at once with `application_stopping`: a running Manager then runs its
  stop sequence (a second stop request forces it as before); a Manager still
  starting ends its start (the host's launch is ended, the listed processes
  are stopped, records are written) and exits with status 70.
- **A read right after a Scene commit shows the commit again (OD-293).**
  Since the Manager publishes its state on its status stream (OD-216), the
  Headless answered a Scene commit before its own view of the logical scene
  had received the new revision, so a read just after the reply could still
  show the previous scene (Load reported a false "Initial scene may be
  incomplete"). `POST /api/scene/transactions` now answers a successful
  commit once that view shows the committed revision, a newer one or another
  active bundle, graph or scene; when the Headless loses its status
  connection to the Manager, or shuts down, the success reply is returned at
  once. A Manager that only missed its status tick (a long commit holds its
  loop) does not end the wait. No time limit applies to this wait.
- **A read right after a Program's pose-observation latch shows it too
  (OD-294).** The Program runner's latch of pose observations into the scene
  between nodes (before a motion, before pick planning, after a
  `simulation.reset` node) went straight to the Manager, so a read just after
  it could still show the previous scene. It now goes through the Headless
  (`GET /api/scene/runtime-owner`, `POST /api/scene/runtime-latch`), which
  answers a latch that committed observations once its view of the logical
  scene shows the latch's revision, under the same rules as a Scene commit;
  a latch that committed none is answered at once. Its refusals keep their
  typed codes. The runner no longer opens the Manager's command socket.

- **A failed run cleanup no longer leaves Reset `stopping` forever (OD-279).**
  When releasing an ended run's resources failed, the run's tick retried it
  every tick (a flood of `program_ticks_late`) and Reset waited without end.
  The cleanup is now retried only by a Reset: the Reset ends failed with
  `program_cycle_recovery_required` (`details.cause_code`
  `program_run_cleanup_failed`), the cycle needs recovery, and each further
  Reset retries the cleanup once; Reset completes once it succeeds.
- **A `hold` rule that trips while a move is being planned holds that move
  (OD-263).** The rule's Stop now also covers the held run's motion in its
  scope that is not sent yet, as the operator's Stop does: the move is
  stopped before sending (no effect), its node holds, and once the rule
  clears Resume plans it again. Before, the move was refused at its send
  with `rule_holds` and its node failed.
- **A held failure of a move that never started offers Continue, Retry and a
  manual result (OD-263).** When the failed operation was already retired
  (for example a `fail_node` rule or an alarm refusing the move before
  sending), the hold kept the retired operation as its owner task, so
  Continue, Retry and the manual result stayed disabled and only Halt ended
  the run. The hold now has no owner task and its offers follow the node's
  recovery policy.
- **A Stop during a motion take-over keeps the run held (OD-264).** A
  take-over newcomer that the Stop ended before it was sent (no effect)
  failed its node with `operation_stopped`, and the run then failed with
  `program_terminal_scope_not_settled`. Its node now holds as for any motion
  the Stop prevented, and Resume makes a fresh attempt. A newcomer whose
  effect is uncertain (`motion_takeover_cleanup_unsettled`) still fails.
- **A Stop during a motion take-over keeps the run held also after the new
  motion started (OD-269).** A take-over newcomer that the Stop ended after it
  was sent (effect settled) still failed the run with
  `program_terminal_scope_not_settled`. The Kernel now keeps its remaining
  motion, as for any stopped motion, and its node holds; Resume plans only
  the rest (a relative move is not applied again). When the rest cannot be
  captured (a Cartesian path stopped before the hand-over, or a send whose
  outcome is not settled), Resume's attempt is refused as Resume of such a
  stopped motion is, with `operation_recovery_invalid`
  (`cartesian_resume_progress_unavailable` or
  `motion_continuation_dispatch_unsettled`).
  Until the old motion's Stop ends, such a newcomer is shown stopping, not
  intervention required, so the run waits instead of failing; it is
  intervention required (`motion_takeover_cleanup_unsettled`) only when that
  Stop ends without settling (OD-272).
- **A Stop during a motion take-over is accepted at once (OD-175).** The
  Kernel recorded a take-over in one step that held the robot's engine and
  participant locks under the lock a Stop needs, so a Stop arriving then
  waited for the whole record. The take-over now checks everything that can
  fail first, records the change under the Kernel lock alone, then completes
  the engine's swap outside it. A new motion that the controller aborts or
  cancels during the hand-over ends as that motion's own Failed or Stopped
  result; the robot is held either way.
- **The Deployment workspace read no longer fails while an Apply publishes
  (OD-265).** `GET /api/deployment/workspace` reads the Manager publication
  once. When that read was not ready it read the execution-ready Scenario as
  well; if the Apply finished between the two reads, that second shape
  made the reply a 500 `composer_internal_failure`. The read now shows the
  publication as not ready, and the next poll shows it.
- **The simulated PLC no longer aborts at exit on a loaded machine (OD-259).**
  `sim_plc` ran its ROS executor on a daemon thread and waited at most 2 s for
  it at exit; when that ran out, Python ended the thread inside rclpy's wait
  and the process aborted. It now stops the executor and waits for the thread
  to end, with no time limit.
- **The Program runner no longer drops records when it stops on a slow
  machine (OD-266).** Its shutdown waited at most 2 s for the event
  follower, 2 s for each of the two liveness threads, 5 s for the tick
  worker, 1 s for the Program-cycle journal and 1 s for the stored values;
  when one ran out, the newest journal records or stored values were not
  written, and a late tick worker failed the shutdown with "Program tick
  worker did not stop". Each step now waits until its thread has ended, with
  no time limit, in the same order. The journal's shutdown line now reads
  `Program-cycle journal records not written in this start: queue full N,
  failed N` (the `not drained` count is gone).
- **Apply offers "Reset run and Apply" again while a run is stopped or held
  (AUD-4).** The run's authoring lock was checked before the Reset-and-Apply
  chain, so Apply stayed disabled with "Program execution is active" and the
  operator had to Reset by hand. The run's lock no longer blocks Apply; the
  chain states its own reasons (for example while robots still move). The
  tooltip now reads "A Program run is active; editing resumes after Reset or
  when the run ends", and a manual motion's says "Editing resumes when it
  ends" (it also covers Deployment edits).
- **A failed Program tick worker is logged, named and fences the cell
  (OD-267).** When a tick worker failed, the Program Runner stopped ticking
  Programs for good but logged nothing, every later New, Execute or Reset was
  refused with a generic code (some saying "try Reset again"), and the cell
  stayed admitted. Now the failure is logged once as
  `manyforge_fault code=program_tick_worker_failed` with its traceback; the
  refusals carry the new code `program_tick_worker_failed` ("Program Runner
  failed": stop ManyForge, start it again and report the logs); and it is a
  Runner fault that fences the cell like a failed BB key publication, but
  never clears until ManyForge restarts, so a running Program is halted. The
  Runner-loss failure predicate `program_runner_publication_fault` is renamed
  `program_runner_fault` and covers both causes; the Kernel's readiness
  decoder now accepts it (it accepted only the three liveness predicates, so a
  Runner fault during a run, the BB publication one included, never reached
  the Kernel and the run was not halted). The alarm "Program Runner
  lost during a run" is now "Program Runner lost or failed during a run" and
  shows its cause message as its headline.
- **The Manager's start-up warning names its cause (OD-261).** Instead of
  "socket is not an exact same-user socket" for every case, it says
  "socket does not exist yet (the process has not started or is not ready)"
  when the socket is missing, which is normal at start, and "socket is not a
  socket owned by this user" otherwise.
- **A Load refused as an incomplete Scenario document names the cause
  (OD-260).** The refusal `scenario_activation_incomplete` now carries flat
  `details`: `member` (`program.yaml` or `deployment.yaml`), and for a Program
  that cannot be built `cause_code` and `path` of its first diagnostic plus
  `diagnostic_count`. The Load dialog shows them, for example "Scenario
  document is incomplete (program.yaml, forest). The Program needs exactly one
  top-level node, …", or "…: deployment.yaml is empty."; before, it showed
  only "Scenario document is incomplete".
- **A refused execution event subscriber is told why (OD-254).** When the
  execution host already served its 8 event-stream subscribers, a ninth saw
  its connection reset ("Connection reset by peer") instead of
  `execution_event_subscribers_full`, because the host closed it without
  reading the subscription it had already sent. The host now reads that
  request first, waiting at most its 100 ms subscribe limit, then sends the
  refusal. The client still logs it and retries with backoff.
- **Load, New and Execute keep working after many Loads in one start
  (OD-256).** Manager's status carried every Apply its journal kept, and after
  some 35 to 40 Loads it grew past its size limit, so Load, New and Execute
  were refused (`manager_projection_unavailable`,
  `program_applied_selection_unavailable`) until restart. It now carries at
  most three Apply records: the running one, the one that installed the active
  configuration and the newest finished one. Older Applies stay readable one
  at a time through `apply_status` and in History; the Deployment panel lists
  the three. A Load whose reply was lost is matched automatically while its
  Apply is among them, otherwise by Recheck. Persistent blackboard values now
  follow the Program the Program runner actually activates; the
  `applied_program_not_observed` fault is gone (the stored-values page loses
  `apply_observation_fault`, `manyforge.persistent_key_values.v3`), the
  committed activation loses `committed_applies`
  (`manyforge.committed_scenario_activation.v6`), and the register file is
  now `persistent-keys.v5.json`: stored values start empty once.
- **A move refused by a global I/O rule just before it starts no longer
  blocks the robot (OD-258).** The move failed with no effect, but its
  operation stayed held until New, so the robot's next move (for example a
  recovery branch) was refused. It now ends at once and the run goes on, as
  for a move refused by an operator alarm.
- **A failed motion is retried automatically again, and its hold offers
  Continue (OD-257).** The Kernel reported a failed execution as
  `retry_ready` (or `reset_required`) while its terminal cleanup still ran,
  before it could be closed, so the Program node took that as final: no
  automatic retry, and a held failure offered Retry but never Continue. A
  failed execution now reports `waiting` (`terminal_cleanup_pending`) until
  its cleanup ends, as a stopped one already did, and the cleanup's end is
  always republished. The Program node waits for a decided assessment, failed
  or stopped, without its former limit of 100 rereads.
- **Starting after an unclean shutdown no longer ends the application (OD-255).**
  Composer read the previous-session report itself and, when Headless had not
  yet filed it, exited, so the Manager ended the application with status 72.
  The warning now comes only from Headless's alarm "ManyForge did not close
  correctly last time", shown once Headless has filed the report; Composer's
  health no longer has `workbench_session_notice` and Headless's health no
  longer has `workbench_previous_session_registration`.
- **A request, in particular a Stop, is no longer dropped when its sender is
  slow to send it.** The execution host closed a connection unread when its
  request did not arrive within 100 ms of the connection being taken (1 s for
  ordinary requests), so a briefly paused sender could lose a Stop. The host
  now waits for each request without a time limit and serves whichever
  connection has sent one, so a connection that has not sent yet holds up no
  other request.
- **ROS is shut down in order when the execution host or the Program runner
  exits.** The execution host left its ROS connection to be ended as a side
  effect of the process ending, after the adapter families' code had already
  been unloaded. The Program runner kept its simulation-control ROS
  connection open when its start failed. Either could crash the process at
  exit. The host now ends its ROS connection explicitly before unloading the
  adapters, and the runner closes its simulation-control connection on every
  exit.
- **An Apply works while an unchanged component's process is dead.** The
  host rebuilt such a component but then checked it as kept, so the Apply
  failed with an unknown effect and authoring stayed blocked until New. The
  Apply now rebuilds it like a replaced component and reserves it, so no
  component action runs on it meanwhile; afterwards the Apply prepares it
  as it prepares a replaced component (nothing is enabled). A failed
  prepare is reported on its row like a replaced component's.
- **New clears every failed Apply that still holds the cell.** New's release
  asks the Kernel what each ended, failed Apply still holds (its claim, its
  fence or both) and releases it, also when the fence was never confirmed to
  the Manager, the Apply failed after publishing its candidate, or an earlier
  release's outcome was unknown; a release whose outcome is unknown is asked
  again by the next New. Before, such an Apply locked authoring until a
  restart (`authoring_not_ready`), and New did not release a claim whose
  release the heartbeat was retrying (later Applies stayed `apply_busy`
  until it settled). Acknowledge is unchanged.
- **Execute no longer stays "Executing…" after Halt, with Resume and Reset
  disabled.** After Halt, Execute resumes the run, whose first tree tick
  holds the motion the Halt stopped; the run can go from halted straight back
  to held without the browser ever seeing it running. Composer waited for
  that running state, so Execute stayed busy and Resume, Run, Retry and Reset
  stayed disabled until the page was reloaded. Composer's pending Program
  commands now clear on the first state that shows the command took effect or
  that the state it was sent from is gone, never on one passing state only:
  Execute (a captured run in any state), Execute after Halt, Halt (a new run
  revision, or the run ending), Reset (a later cycle), group Run and Resume (a
  newer hold revision). An unconfirmed Halt still waiting for
  motion is dropped when the run is halted or ends, and a Reset waiting for
  motion to stop asks its question once the run has ended.
- **An invalid Stop stops the cell instead of nothing.** A Stop whose body
  or payload is invalid (on the Stop HTTP routes, the Program group Stop
  route, the control WebSocket or `/api/control/commands`), whose fields are
  invalid (targets unsorted or repeated, an empty, oversized or NUL-containing
  id or label, an out-of-range expected revision), whose request key or
  attribution is invalid, or which names an unknown target was refused and
  stopped nothing. It is now accepted as Stop all (`widen_reason`
  `malformed` or `unknown_target`); an invalid Halt is accepted as a Halt.
  An invalid attribution is recorded as unverified. On the control WebSocket
  and `/api/control/commands` an `operation_stop`, `stop_all` or `halt` no
  longer needs `expected_execution_owner_epoch`. Other commands with an
  invalid payload are still refused. The error codes
  `normalized_stop_intent_invalid`, `normalized_full_stop_intent_invalid` and
  `stop_request_attribution_invalid` no longer exist.
- **The assistant can be enabled again.** The ROS overlay's installed scripts
  ran under the image's system Python instead of the application's virtual
  environment, so the assistant host started its bridge without a module the
  bridge needs and Enable failed with "controller launch exited with status
  0". The point-cloud source's observer bridge had the same defect. The
  overlay is now built with colcon running under the virtual environment's
  Python.
- **Mock inputs can be set from the Tool & I/O panel again.** With a Scenario
  loaded, the panel took the Deployment Draft for unapplied (a Scenario
  Deployment carries no revision), so it offered no **Set true** / **Set
  false** button and no declared-source hint, for example for
  `curtain_blocked` in `mock_lite6_light_curtain_rule`. It now uses the same
  Draft-equals-Applied check as the Deployment actions.
- **A move at a mounted tool's TCP no longer needs the Program to name the
  tool.** A Program that never referenced the robot's mounted gripper (or only
  commanded it) failed its first **Current** move with "TCP target/frame
  association is absent or ambiguous" (`runtime_motion_endpoint_unresolved`),
  for example `mock_lite6_motion_preemption`, `mock_lite6_motion_retry`,
  `mock_lite6_intervention_hold` and `mock_lite6_light_curtain_rule`. A move's
  TCP, in every selector form, now resolves its frame and contact bodies from
  the moving robot's TCP list, on every lane. The dual-arm examples use
  **Current** again.
- **Apply checks TCP selectors.** A specific TCP that is not on the moving
  robot's list (`active_tcp_unknown`), whose tool is not mounted on that robot
  (`tcp_tool_not_mounted`) or whose frame matches several entries
  (`runtime_motion_endpoint_ambiguous`), and **Default TCP of** a component
  without a default TCP (`default_tcp_unavailable`), are reported by reference
  validation and refuse Apply, instead of failing only when the move runs.
- **Gripper status nodes work on a gripper running as its own component.**
  **Check gripper status** and **Wait for gripper status** failed with "target
  has no unique qualified observation owner"
  (`execution_target_observation_unavailable`), and a pneumatic gripper was
  always refused. They now report the command state: for a position gripper,
  reached is the controller's result; for a pneumatic gripper with a valve
  output, reached is the delivered output plus the dwell, without a finger
  position. A pneumatic gripper that is display-only or switches its valves
  through mapped I/O has no command state: Apply refuses the node
  (`gripper_status_unavailable`). `mock_lite6_pick_and_place` and
  `mock_ur10e_pick_and_place` check the gripper's status after opening it.
- A **Default TCP of** a component without a default TCP now fails with
  `default_tcp_unavailable` instead of `end_effector_tcp_unavailable`.
- **A Stop during a goal send can no longer hang an arm's motion.** When a
  Stop reached a goal before the controller accepted it, and the acceptance
  arrived while another goal was being sent to the same controller, the two
  could wait on each other for good. The cancel that becomes due on acceptance
  is now sent from the Stop lane, never from the acceptance callback; if it
  cannot be scheduled, the component reports a fault.
- **A Stop that arrives while a motion is being started now ends correctly.**
  A Stop that came after a move was released to its robot but before the
  move was sent (while the robot's tool data was applied, the collision
  monitor was prepared or the goal was about to go out) kept the robot still,
  but ended as failed ("Addressed Stop names a stale prepared motion"), and
  every later Stop failed the same way until a restart. Such a Stop now waits
  until the start step has finished and then ends as what really happened:
  stopped if the goal had gone out, cancelled before the start with no
  effect if the Stop came first. The robot is held at once either way; only
  the Stop's own ending waits, for as long as the tool-data step and the
  monitor preparation take. Manual moves and the reset-position move behave
  the same. A goal that such a Stop holds back while it is being handed to the
  controller is now refused as `execution_port_stop_in_flight` (before:
  `execution_port_submit_refused`). When a Stop still names an outdated motion, the error's details
  now name each field that did not match (`mismatched_fields`, with the
  motion's `session_state`, `expected_session_state` and `dispatch_gate`).
- **A motion that takes over from a running one no longer stays pending for
  good when a Stop cancels the take-over.** When the new motion might already
  have been sent and the old motion's Stop was not confirmed (for example
  because that Stop failed and halted the cell), the new motion stayed
  pending and could be neither recovered nor reset. A take-over cancelled by
  any Stop now ends such a motion at once as intervention required with its
  effect uncertain (`motion_takeover_cleanup_unsettled`); the operator
  recovers or resets it. A new motion that was provably not sent still ends
  as cancelled before the start with no effect. A motion queued behind it
  also ends as cancelled before the start by that Stop when the take-over's
  own Stop failed (before: failed, `predecessor_handover_failed`).
- **Stop all, Halt and the cell Stop no longer send stop commands again to a
  robot whose stop is already confirmed while its operation waits for the
  operator;** the released version stopped such a robot again.
- **A Program Stop is no longer delayed by the disk, and is sent even if its
  record cannot be written.** The Program runner wrote "stopping" to its cycle
  journal and waited for the disk before it sent the Stop of a running
  Program, and a failed write meant the Stop was not sent at all. It now
  sends the Stop first and records it afterwards, as Program Full Stop
  already did; a failed record is logged and does not affect the Stop.
- **The Program runner can no longer hang for good when the Manager's
  heartbeat returns after a pause while a command is in progress.** The
  heartbeat that reconciles the runner with the Manager waited for the
  running command while holding the runner's liveness state, and a second
  waiting command then waited for that state: neither could go on, and no
  further command, heartbeat or Manager-loss Stop was handled. The heartbeat
  now waits without holding that state; if the runner's liveness changed
  meanwhile, the heartbeat is refused and the Manager's next one reconciles.
- **A Stop or take-over on a real robot no longer speeds the arm up for an
  instant.** Its replacement trajectory started at a fixed 20 ms guess of
  when the controller would take it, 12–17 ms further along the path than
  the arm was, so the controller jumped ahead for one cycle (3–5 mrad at
  moderate speed) and the arm sped up to catch up. Real lanes now stamp the
  replacement with a start time on the controller's own clock, as Mock
  already did, so it starts exactly where the original is. Isaac still uses
  the estimate.

## 0.2.1 – 2026-11-10

### Breaking changes

- **The AI assistant is a Deployment component.** The chat bridge runs only as
  the process of an `assistant` component that references an `ai_model`; the
  workbench and operator launcher no longer start it, port 8100 and
  Composer's `--assistant-bridge-url` are gone. A Deployment needs an
  assistant component for the AI tab. See the assistant cookbook.
- **Assistant formats:** `maintained_assistant_profiles.v2.json`,
  `manyforge.assistant_mode_projection.v2`, `manyforge.assistant_mode_index.v2`,
  `manyforge.assistant.bridge_health.v2`,
  `manyforge.composer_assistant_profile_aliases.v2` and
  `manyforge.retained_assistant.v3`. A Deployment applied by an earlier build
  is refused until it is loaded again.
- **Motion defaults bind by name.** A motion field inherits the Program
  parameter of the same name; a renamed parameter (for example
  `cycle_velocity`) is no longer a motion default.
- **Component types without `.fixed_scale` variants**, a required
  `runtime_scale` on every robot lane row, and the removed
  `kernel.set_runtime_scale` node and `set_operation_runtime_scale` command.
- **Removed Deployment keys:** `gripper_result_timeout_s` (lane) and
  `delivery_result_timeout_s` (gripper target); a Deployment that sets them is
  refused.
- **Reference validation request** is
  `manyforge.program_reference_validation_request.v3`.
- **Error codes:** `response_rejected` and `composer_draft_stale` are retired
  (an unknown assistant tool is the per-call `unknown_tool`; stale Drafts are
  `composer_draft_revision_stale` or `composer_workspace_stale`).

### Added

- **The AI assistant is a component** (kind `assistant`,
  `manyforge.assistant`, at most one per cell; Add component > AI assistant).
  Its row names the chat's `ai_model` (`model_component_id`) and an assistant
  profile with a chat mode (`assistant_profile_id`). Prepare starts its host,
  Enable starts the bridge on a loopback socket with an ephemeral port and
  publishes the URL, Disable stops it (running requests end `cancelled`, the
  history is dropped); activity is `busy` during a request, the `bridge`
  stream shows its health, and a host or bridge exit is `fault_process_exited`.
  The AI tab says why it is disabled. Chat and Program runs never overlap:
  Composer refuses a chat request while a run exists
  (`assistant_program_active`) and Execute while a chat request runs
  (`execution_assistant_turn_in_progress`). New refusals:
  `assistant_component_absent`, `assistant_component_not_enabled`,
  `assistant_ai_model_not_enabled`, `assistant_component_duplicate`,
  `assistant_profile_has_no_chat_mode`. See the
  [assistant cookbook](docs/operations/ASSISTANT_E2E_COOKBOOK.md).
- **`ComponentState.endpoints`** (`[{role_id, url}]`, only while enabled):
  the URLs a component serves; clients may act on them, unlike `details`.
- **The assistant declares Program contact policies.** The `composer-assistant`
  mode offers `program_draft_upsert_contact_policies` and
  `program_draft_remove_contact_policies` (26 tools), the Draft edits of the
  Program contact policy editor, so it can author a planned pick
  (`planning.plan_pick_sequence` needs a `contact_grasp` policy). The edits
  stay in the Draft for review and Apply. See
  [Assistant provider contract](docs/reference/ASSISTANT_PROVIDER_CONTRACT.md#mode-and-tools).

### Changed

- **Motion defaults inherit by name; inserts keep what is set; copies keep
  their internal references.** A motion node's `velocity_scale`,
  `acceleration_scale`, `translation_tol_m` and `blend_radius_m` inherit the
  Program parameter of the same name, field by field. A Program parameter
  with another name (for example `cycle_velocity`) is no longer recognised as
  a motion default. Inserting a node or subtree (also replace, change kind
  and the Pick/Place subtree) keeps every literal and value source it brings,
  at every depth, and binds only unset fields; the palette and change kind
  leave the four fields unset. Copy, Paste and Duplicate rename
  `node_outcome.<node>` references between the copied nodes. These fields may
  now read any numeric value source; when the move starts, a value above the
  Program `velocity_scale`/`acceleration_scale` refuses the move with
  `motion_value_above_program_limit` (never clamped). The Motion defaults
  panel works per declared field and counts dynamic nodes;
  `program_draft_update_motion_defaults` takes only the declared fields and no
  `parameter_bindings`. See [Motion defaults](docs/authoring/MOTION_DEFAULTS.md).
- **Assistant formats**: the mode projection
  (`manyforge.assistant_mode_projection.v2`) names `assistant_component_id`
  and each mode's `runs_in` instead of `provider`; the mode index
  (`manyforge.assistant_mode_index.v2`) adds `assistant_component_id` and is
  empty without an assistant component; the profiles file is
  `maintained_assistant_profiles.v2.json` (no `provider_alias`); bridge
  `/health` is `manyforge.assistant.bridge_health.v2` `{status, activity}`;
  `GET /api/deployment/assistant-profiles` is
  `manyforge.composer_assistant_profile_aliases.v2`
  (`{profileId, chat, modelEndpointAliases}`). The Deployment-level
  `assistant_profile_id` now only carries runtime prompt templates and model
  aliases; the chat alias `assistant_model` is bound by the assistant
  component and refused in `assistant_model_components`. A saved Deployment
  with a Deployment-level chat profile and no assistant component loads
  without chat; nothing is converted.
- **The assistant sends no sampling parameters by default** (it sent
  `temperature: 0`), so the model's or server's defaults apply. The assistant
  profile's optional `request_contract.sampling: {temperature?, top_p?,
  top_k?}` sends exactly the fields it sets; out-of-range values are refused.
  See [Assistant provider contract](docs/reference/ASSISTANT_PROVIDER_CONTRACT.md).
- **A constrained regeneration does not think.** With `reasoning: on`, the
  assistant's one-call argument repair sends
  `chat_template_kwargs: {enable_thinking: false}`.
- **Reference validation sends model sources by digest.** The Composer sends
  `model_source_closure_digest` instead of the full model-source closure when
  Headless holds the prepared candidate (request
  `manyforge.program_reference_validation_request.v3`, exactly one of the two
  fields); Headless refuses an unknown digest with `model_sources_unknown` and
  the Composer resends the full closure once.
- **An emergency stop is a normal event on UFACTORY machines.** Load (Prepare)
  only connects, works with the e-stop pressed, and leaves the latched
  controller error (C1–C3) as reported: a stop that gates motion, not a fault.
  Clearing it is explicit: Recover the machine, or Enable the arm. Enable is
  refused with `robot_estop_active` while the e-stop stays pressed, and a
  member's Enable or Reset position with `owner_estop_active` while its machine
  is stopped. **Enable all** recovers a stopped machine first and prepares what
  is not prepared. See
  [Troubleshooting](docs/operations/TROUBLESHOOTING.md).
- **Speed changes apply to motion already running.** A new speed value goes
  straight to the joint trajectory controller's speed scaling input with a
  rate-limited ramp; the motion's time limit follows the applied speed.
- **Grippers on `parallel_gripper_action_controller`** (ros2_controllers). A
  command completes on its action result, not on feedback.
- **ROS 2 Lyrical built from source** (sync 2026-10-05) in the workbench image,
  with ros2_controllers 6.10.1 and ros2_control_cmake 1.0.0; the UFACTORY
  driver is pinned to the fork commit `3654c9a`.
- **AI assistant panel** restored to the 0.1.0 layout as a dock: conversation
  with Draft-change summaries, the model in use, orientation fields and tool
  calls; it stays open beside the 3D view and keeps the conversation when
  closed.
- **The node catalog documents every node.** Each entry says what it does and
  when it completes or fails; every parameter and every value type
  (`schema_types`) has a description. Parameters and type properties accept
  `description`; a catalog test fails when one is missing.
- **The AI assistant sees nodes through the catalog.** It may author every
  stable node whose capabilities the active Deployment offers (the palette's
  nodes; no per-mode node list). Headless builds the node and tool
  descriptors from the loaded catalog at startup; the stored
  `assistant_instrument_registry_source.v1.json`, its generator and the
  profile `nodes` list are gone. The prompt lists `id: summary` per node; the
  tree tools take one generic node shape and are a few KB instead of about
  60 KB each; `catalog_read` returns full descriptions, parameters and the
  value types they use.
- **The assistant's input is generated and concise.** The system prompt is
  rules, the cell, an outline of the Draft and the nodes by category (about
  2k tokens instead of about 20k with the full Draft JSON); tools are
  top-level (about 13 KB instead of 45 KB) and name their argument types,
  which `catalog_read` resolves; `program_read` reads one subtree by
  `instance_id`; edit results show `{ok, changed, diagnostics_new, diagnostics_other}` (diagnostics this edit caused, and the count already open). The bridge
  checks shapes and types only: an incomplete node is a Draft diagnostic, as
  in the GUI.
- **The assistant sees the cell's real ids and rules.** Composer runs one reference validation per assistant request; the prompt lists each component's targets, frames, input and output signals and mount, and the Deployment's interrupt rules, from the candidate's selector facts, which gain `rules` (`rule_id`, `scope`, `on_trip`, `active_at_start`, `condition`).
- **Assistant edits report reference diagnostics.** A committed assistant edit returns `referenceDiagnostics` from the same reference validator as the GUI (OD-28); the model sees the new ones in `diagnostics_new` and counts the open ones, including those open at the request's start.
- **Assistant argument errors name the field.** A rejected call gives
  `assistant_tool_arguments_invalid` listing every error with its field path
  and expected shape, e.g. `node.parameters.position: expected
  coordinate_position {…}`. Argument text that is not valid JSON quotes where
  it breaks (or the trailing text to remove); a placeholder such as `""` or
  `"null"` for an optional id says to omit it; a type name in
  `catalog_read entry_ids` points to `types`; `@root` given while the Draft
  has no single root node names the top-level nodes.
- **Stuck-loop recovery** (mode setting `stuck_loop_recovery`, default on):
  the second same-class failure of a tool adds a typed `recovery` hint to the
  tool result; the fifth ends the request with a clarification.
- **Node catalog matches what the nodes do:** `io.wait_for_input` `edge`
  offers only `level`; `assistant.build_scene_selection` offers only
  `image_source: none` and no longer lists the unused `image_snapshot_*`
  parameters; `assistant.query_model` `timeout_s` has no catalog default, so
  an omitted value uses the endpoint's `default_timeout_s`; `timer` declares
  its failure outcome (`input_unavailable`); the pick synthesis and motion
  option types carry their defaults (catalog schemas accept `default`).
- **The gripper node owns its completion deadline.** `gripper.command_position`
  fails with `gripper_result_timeout` when the gripper has not reported done
  `completion_timeout_s` after `command_duration_s` (pneumatic: after the
  delivery alone). The default is 5 s (was 30 s, never enforced);
  `tool_completion_timeout_s` of the Pick and Place planning nodes follows the
  same rule. The component lane key `gripper_result_timeout_s` is removed; the
  Robotiq example Programs set 15 s.
- The unused gripper target key `delivery_result_timeout_s` is removed from
  Deployments; a Deployment that still sets it is refused.
- **Assistant nodes need a usable model query.** `assistant.query_model`,
  `assistant.build_scene_selection` and `assistant.validate_scene_selection`
  require the new cell capability `assistant.model.query`, offered when the
  Deployment's assistant settings have a model endpoint and at least one
  prompt template. The palette, the AI assistant and Program validation
  derive it like any other capability; the palette reason
  `model_not_configured` is replaced by `capability_not_offered`.
- Retained assistant settings are `manyforge.retained_assistant.v3`; a
  Deployment applied by an earlier build is refused until it is loaded again.
- **A robot's lane row declares `runtime_scale: supported | unsupported`**
  (required in every robot `lanes.<lane>` row; a tool row has none): whether
  its adapter's arm controller accepts runtime speed scaling. The arm's
  runtime-scale qualification in the Deployment follows the declaration, and
  so does the lane's speed scaling. Load refuses with
  `component_runtime_scale_mismatch` when the declaration differs from the
  generated arm controller (only a position-only JointTrajectoryController
  accepts speed scaling). The Mock, Isaac and xArm rows declare `supported`;
  behaviour is unchanged.

- Reference validation reuses its prepared candidate while the Deployment and
  its model sources are unchanged (one cached candidate, replaced on change),
  so a Program edit re-runs only the reference checks.
- **Native builds run before the workbench starts, not inside its startup
  timeout.** `scripts/compose-dev.sh start` (workbench and operator runtime)
  first runs `python3 scripts/dev-workbench.py build` in the workbench image;
  `MANYFORGE_DEV_START_TIMEOUT_SECONDS` then bounds readiness only.
  `compose-dev.sh build-native` runs that step alone; the Isaac operator runs
  it as its own call (45 min bound) before start. `serve` no longer builds: it
  refuses with the build step to run when a native artifact is missing. Apply
  and Load never build.
- **Assistant Draft refusals keep their typed code.** A refused assistant
  Draft call's `validation_error` result carries `detail.code` (for example
  `draft_arguments_invalid`, or `assistant_callback_arguments_invalid` for
  Composer's schema check). The model's tool result shows that code, and
  stuck-loop recovery counts repeats per code instead of per
  `validation_error`.
- **The assistant prompt's Draft outline lists contact policies** (`id: mode`,
  each zone's body pairs and its maximum penetration or release evidence), so
  the model no longer needs `program_read` to see them.

### Removed

- **The launcher-started assistant bridge**: the workbench and the operator
  launcher no longer start it, port 8100 is no longer used, and Composer's
  `--assistant-bridge-url`, `MANYFORGE_ASSISTANT_BRIDGE_HOST`/`_PORT`,
  `components.assistant_bridge` in Composer's `/api/health` and the
  `direct`/`nemoclaw` provider kinds are gone.
- **`manyforge_assistant_bridge/supervisor.py`**, the unused availability
  reducer of the superseded assistant supervisor.
- **The `kernel.set_runtime_scale` Program node** and its capability
  `execution.runtime_scale.operation_scoped`. The speed control in the GUI
  (runtime scale control) is unchanged.
- **The `.fixed_scale` component-type variants.** Each robot and tool has one
  component type, `<model>.component`; the compiler selects it directly.
- **The operation-scoped speed command `set_operation_runtime_scale`**
  (execution host, Headless and Program Runner clients, Kernel
  `set_runtime_scale`). Speed changes go through runtime scale control.

### Fixed
- The operator launcher creates its state directories owner-only, so it no
  longer refuses a directory it created itself on hosts whose umask is 0002.
- Program reference validation works in the workbench and the operator
  runtime: the launcher builds the Core Python module that Headless uses for
  it, and Headless accepts candidate-sized reference-validation requests (the
  Deployment's model sources made them exceed the standard body bound).

- Every AI assistant request failed with `assistant_projection_stale`; scene
  tools offered stale frame references.
- Experimental constrained regeneration (`constrained_regeneration`): an
  upstream failure of the regeneration call (e.g. vLLM refusing an oversized
  grammar with HTTP 400, or a timeout) no longer ends the request; the
  original call and its error stand. A node without a known `node_id` and
  further children use a compact generic node shape, and container children
  carry no item bound in the grammar (the validator still enforces it), so
  the grammar stays small.

## 0.2.0 – 2026-11-02

0.2.0 rebuilds how a cell is modelled and run: every lane runs on ROS 2 with
the stock joint trajectory controller, every component follows one state
model, and planning moves to RoboPlan 0.8. Files written for 0.1.0 do not
load; start from the migrated examples in `examples/`. ManyForge is still not a
safety system ([Safety](README.md#safety)).

### Breaking changes

- **Direct Scenario references.** Older Scenario projects using assignments
  must be re-authored with direct component, target, frame, and signal references.
- **Component model v2.** Every component reports one state (lifecycle
  offline / not prepared / prepared / enabled / fault, activity idle / busy /
  unknown, stream freshness) and offers the actions Prepare, Enable, Disable
  and Recover. See [Component model](docs/reference/ARCHITECTURE.md#component-model).
- **Machines: owners and members.** A vendor machine (`ufactory.lite6_machine`,
  `ufactory.uf850_machine`) is an owner (driver process, connection,
  controller errors, power) with members (arm, controller I/O, tool I/O). A
  UFACTORY robot is always placed as a machine; members can be deactivated
  (`active: false`), not removed. See
  [Machines](docs/reference/ARCHITECTURE.md#machines-owners-and-members).
- **Derived cell latch.** The cell latches only when a component is in fault,
  offline or of unknown activity after a Stop, or faults while something
  executes; it clears by itself when every component is healthy and idle.
  Program nodes, a lost grasp and interrupt rules never fault the cell. See
  [Faults and the cell latch](docs/reference/ARCHITECTURE.md#faults-and-the-cell-latch).
- **One readiness result** gates Execute and Resume (`GET /api/program/readiness`,
  `/ws/control` key `run_readiness`): every active component enabled, no
  latch, no rule holding, required inputs fresh. See
  [Run readiness](docs/reference/ARCHITECTURE.md#run-readiness).
- **Load and Apply leave components prepared, never enabled**, on every lane.
  Press Enable, **Enable all** or **Activate and execute**; scripts, CI and
  MCP clients must call Enable after Load.
- **All lanes on ROS 2 Lyrical** with `ros2_control` and the stock joint
  trajectory controller 6.10.0: Mock (mock hardware), Isaac Sim (attach-only)
  and Real (UFACTORY xArm family). See
  [Deployment lanes](docs/reference/ARCHITECTURE.md#deployment-lanes).
- **Native lane removed:** the non-ROS replay runtime
  (`manyforge.native_execution_runtime`), its settings, Apply path, fixtures
  and the MoveIt jerk import (`--import-moveit-jerk`). Mock on ROS replaces it;
  the last commit that contains it is `0703ea48` on `dev`. "Native runtime" is
  now "execution host".
- **Close removed.** **New** empties the cell and always works; **Load** is
  New, then Apply of the chosen project. See
  [Load, New, Activate, Execute, Run](docs/operations/DEVELOPMENT_WORKBENCH.md#load-new-activate-execute-run).
- **Deployment changes** (`manyforge.deployment_intent.v2`): machine members
  with `owner`, `role` and `active`; Stop settings per robot target under
  `components[].target_settings.<target>.controlled_stop`; attachment
  `collision_exclusions`; planner, clearance and Cartesian timing defaults per
  robot. Removed: per-stream `severity` and `recovery` (only `budget_ns`
  stays), `policies.interrupt_config`, `interrupt_sources`,
  `interrupt_policies`, `idle_behaviour` and `full_stop`.
- **Program changes** (`manyforge.program.v2`, node catalog 2.9.0):
  `planning_profile` is replaced by the `planner` parameter,
  `max_blend_deviation_rad` by `blend_radius_m`; the robot-role `tcp` frame
  alias is gone (one name per frame). The 0.1.0 formats
  (`manyforge.program.v0`, `manyforge.deployment.v0`, loose files) are
  replaced by Scenario projects.
- **Removed endpoints** (Composer and Headless): all `/api/cclc/*`,
  `/api/component-overview`, `/api/execution/stop-fault` and
  `/stop-fault/recover`, `/api/execution/cell-recover` (and `/abort`),
  `/api/execution/force-close`, `/api/manager/canonical-empty-selection`.
  Composer only: `/api/activation-preparations`,
  `/api/deployment/draft/preview` and `/draft/apply` (now
  `/api/deployment/preview` and `/apply`), `/api/scenario-documents/load` and
  `/new` (now `/api/scenario/load` and `/new`), `/api/manager/apply` and
  `/api/manager/deployment-candidates/preview`. See the
  [Composer](docs/reference/REST_API.md) and
  [Headless](docs/reference/HEADLESS_REST_API.md) contracts.
- **Site settings are required for the Real lane:** robot addresses live in a
  `manyforge.site.v1` file (`MANYFORGE_DEV_SITE_SETTINGS`, read at start); a
  real machine names its key (`configuration.device_address_ref`), never the
  address. See [Real robot setup](docs/operations/REAL_ROBOT_SETUP.md#site-settings-the-robots-address).
- **xArm driver from the ManyForge fork:** the image builds
  `pastoriomarco/xarm_ros2` branch `lyrical-no-gazebo` at commit
  `46aa1147d99963a8357b89744633db428746667b` (upstream `lyrical` plus the
  `xarm_io_states` topic and the `system_control` service). The upstream
  driver lacks both. See the [workbench guide](docs/operations/DEVELOPMENT_WORKBENCH.md#install-one-build-one-start).
- **Stop modes and settings:** up to two fallback modes after the chosen one
  (`soft_on_path` → `spring_back_hold` → `spring_back_fast`), then always a
  hard stop; deceleration time default 1 s, at most 2 s; per-joint deceleration
  is written to every lane's controller (`max_deceleration_on_cancel`), so
  Isaac now uses the Deployment's values. See
  [Controlled Stop profiles](docs/operations/STOP_PROFILES.md).
- **Recovery and acknowledgement:** nothing acknowledges a Stop or a fault.
  Recover cell, Force close, Stop-fault recovery and the operator's "clear
  latch" are gone; Recover the faulted component and the latch clears.
  Component faults and the latch never clear by acknowledgement. See
  [Recovery, acknowledgement, and alarms](docs/reference/ARCHITECTURE.md#recovery-acknowledgement-and-alarms).
- **Global interrupt rules:** a trip is the operator's Stop on the rule's
  scope, then gates Resume, Execute and manual motion while the condition
  holds. "Full Stop, operator clears", the `latched` key state, the source-loss
  latch and "Motion waits" are removed; `active_at_start` defaults to `false`.
  See [Interrupts](docs/authoring/INTERRUPTS.md#4-global-rules-deployment).
- **Launcher defaults:** profile `native` is now `standard`; `--instance-id`
  defaults to `workbench` (state folder and journald tag follow it) and
  `--cell-id` to `cell`. Stop a running older workbench before the first
  0.2.0 start.
  See the [operator runbook](docs/operations/MANYFORGE_OPERATOR_RUNBOOK.md).
- **Journals reset:** run history, Stop and operation journals from earlier
  builds are not read or converted; History starts fresh.

### Added

- **History records component actions and Applies.** Every component action
  (`component_action_finished`) and every Scenario Load/New, Deployment
  Apply/Revert and Program Apply/Revert terminal (`apply_finished`) appears in
  History with its outcome and typed error code and message, including actions
  refused before they started. See
  [Runtime events](docs/operations/OBSERVABILITY.md#runtime-events).
- **Component build spec:** each component is built only from its spec; its
  fingerprint is the spec digest, so Apply restarts a component exactly when
  something it is built with changes. See
  [Component build spec](docs/reference/ARCHITECTURE.md#component-build-spec).
- **Planner block:** `search` (`rrt`, `rrt_connect`, default `rrt_connect`),
  `optimize` (`first`, `rrt_star`, `best_of_n`), collision check step and node
  budget; Deployment default with a per-motion `planner` override. See
  [Planner block](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#planner-block).
- **Corner blending:** `blend_radius_m` per Follow Cartesian path move
  and per waypoint rounds corners so the tool runs through them; 0 keeps the
  exact corner. See [Cartesian paths](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#cartesian-paths).
- **Cartesian timing:** `cartesian_timing` `constant_speed` (default)
  or `time_optimal`, per move with a Deployment default.
- **TCP limit inside time scaling:** every move is timed to keep the
  active TCP within 0.98 of its speed limit; the speed check runs on the
  active TCP only. See
  [Time-scaling and the TCP speed limit](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#time-scaling-and-the-tcp-speed-limit).
- **Motion timing:** a per-robot Deployment setting
  `motion_timing` for joint and pose goals, shown in the Execution host
  configuration card. `smooth` is the default: TOPP-RA within the joint
  limits, then one uniform stretch under the TCP limit (gentle
  starts and stops). `fast` puts the TCP bound inside TOPP-RA,
  quicker with abrupt acceleration changes. Cartesian paths follow
  `cartesian_timing`. Changing it restarts nothing.
- **Collision clearance:** planned and validated moves keep the
  Deployment's `collision_clearance_m` (default 1 mm) from obstacles;
  per-move `clearance_m` overrides it. See
  [Collision validation](docs/reference/ARCHITECTURE.md#collision-validation).
- **Check fresh plans before dispatch:** a per-robot Deployment
  setting `collision_check_fresh_plans` (default on), shown in the Execution
  host configuration card: ManyForge's collision engine checks every waypoint
  of a newly planned move before it starts. Changing it restarts nothing. See
  [Collision validation](docs/reference/ARCHITECTURE.md#collision-validation).
- **Mount collision exclusions:** arm–tool pairs that can never touch are
  listed on the attachment (`collision_exclusions`) and not checked; the
  Lite 6 and UF850 examples carry them. See
  [Mount collision exclusions](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#mount-collision-exclusions).
- **Contact pairs per request:** the active contact-grasp pairs travel with
  each plan and IK request, so a contact-policy change keeps the planner
  worker. See [Planner scene](docs/reference/ROBOPLAN_ADAPTER_NOTES.md#planner-scene).
- **Gripper Stop near the target:** a gripper already within its
  `stop_finish_tolerance` finishes its goal instead of being cancelled. See
  [Stop near the target](docs/authoring/GRIPPERS.md#stop-near-the-target).
- **Wait for the set duration:** `gripper.command_position`
  `wait_for_duration` completes after `command_duration_s` on every lane. See
  [When a gripper command completes](docs/authoring/GRIPPERS.md#when-a-gripper-command-completes).
- **Real xArm motors on only at Enable:** Prepare connects and clears
  errors with the motors off; the arm's Enable switches them on. Machine
  actions **Power off** and **Reboot**. See
  [What each action does on an xArm](docs/operations/REAL_ROBOT_SETUP.md#what-each-action-does-on-an-xarm).
- **Reset robot position on Isaac:** Isaac robots declare
  `reset_position`, like Mock: the trajectory controller off, the
  Deployment's initial joints written once through the stock writer to
  Isaac's joint commands, the controller back on to hold them. Refused with
  `simulation_not_playing` while the simulation isn't playing. See
  [Isaac lane](manyforge_isaac_adapter/lane/README.md).
- **xArm I/O from the controller's real-time report** (`xarm_io_states`,
  100 Hz): controller and tool I/O as machine members, controller-owned
  outputs read-only.
- **Components page:** component list and machine cards with state, next
  action, Prepare all / Enable all and the controller's status details.
- **Deployment Preview → Apply:** the Preview lists each component's
  disposition and whether it restarts; Apply restarts only those. Fields show
  effective values with their defaults (`GET /api/deployment/effective`).
- **Tool & I/O tab:** each signal's observed value, source and age, and manual
  output writes. Programs and manual writes share one write path
  (`POST /api/io/{signal_id}/set`).
- **Outside changes as a signal:** `<output>.changed_outside` for outputs with
  read-back, and the **Clear outside change** node. See
  [Signals](docs/authoring/INTERRUPTS.md#signals).
- **Rule control:** the **Set rule active** node, `active_at_start`, and per
  rule after the Stop `hold` or `fail_node`; Notify publishes
  `interrupt_state.<rule_id>`. See [Interrupts](docs/authoring/INTERRUPTS.md).
- **Vendor-neutral fault codes** (`estop`, `protective_stop`,
  `limit_violation`, `drive_fault`, `communication`, `external_control`, …)
  with the vendor's original code alongside.
- **Point-cloud source component** (`manyforge.pointcloud_source`) on every
  lane, shown in the 3D view.
- **Ordinary-motion jerk limits** on every lane when the Deployment switch is
  on (planned by RoboPlan; nothing extra is sent to ROS).
- **Tools as components:** grippers mounted on a robot flange, per-component
  TCPs and default TCP, the **Active TCP** and the **Set TCP** node; payload
  follows attach and detach. See [TCPs](docs/authoring/TCPS.md).
- **Pneumatic grippers on I/O:** valve outputs and optional open, closed,
  pressure and workpiece-grasped inputs from any I/O component; each gripper
  node picks its `required_inputs`. See [Grippers](docs/authoring/GRIPPERS.md#pneumatic-grippers).
- **Grasp lost** as a Program event (`on_grasp_lost`): Stop, detach at the
  observed pose, typed outcome. See
  [Grasp lost](docs/authoring/INTERRUPTS.md#grasp-lost-as-a-program-event).
- **Component streams:** per-stream freshness budgets; a robot's or I/O
  device's stale stream is a fault, a sensor's only shows stale.
- **Isaac pose source** as a `pose_source` Deployment component, with
  per-object pose settings on the Object tab.
- **Measured ghost:** the live viewer shows the measured robot pose with its
  freshness.
- **Robots as data:** component descriptors and packages under `components/`.
  See [Adding a component](docs/authoring/ADDING_A_COMPONENT.md).
- **Composer Scenario projects** (`scenario.yaml`, `deployment.yaml`,
  `program.yaml`, `assignment.yaml`) with Draft, Apply, Discard, Save, Save
  As, Load and Export; project folders outside the repository from the site
  file. See [Project folders](docs/operations/DEVELOPMENT_WORKBENCH.md#project-folders).
- **Programs:** persistent Blackboard keys, expressions, node rules (Move
  until signal, Probe search) and automatic motion retries. See
  [Blackboard keys](docs/authoring/BLACKBOARD_KEYS.md) and
  [Expressions](docs/authoring/EXPRESSIONS.md).
- **AI model components:** `ai_model` Deployment components (several per
  Deployment) for the Composer AI tab; tokens only from `MANYFORGE_AI_*`
  variables. See the [assistant cookbook](docs/operations/ASSISTANT_E2E_COOKBOOK.md).
- **Runtime history** with a disk bound and up to five previous sessions;
  logs, evidence and motion traces are disk-bounded too. See
  [Observability](docs/operations/OBSERVABILITY.md).
- **Development workbench:** `./scripts/compose-dev.sh build` and
  `start` / `status` / `stop` on one ROS 2 Lyrical image for every lane. See
  the [workbench guide](docs/operations/DEVELOPMENT_WORKBENCH.md).
- **Example Scenarios** for Mock (`mock_*`), Isaac (`sim_*`) and Real
  (`real_*`), including Lite 6 pneumatic pick and place on the real robot.

### Changed

- **RoboPlan 0.8.0:** constant TCP speed on linear moves; free-space paths
  differ for the same seed; IK restarts randomize only the group's joints.
- **Rounded Cartesian corners are resolved finely:** with `constant_speed`,
  a move with a blend radius is resolved at least every twentieth of the arc
  radius, so it no longer slows down as a whole.
- **Pre-dispatch collision check:** runs on the trajectory's own waypoints
  (the clearance covers the spline) for cached moves, moves with a contact
  policy and, unless the robot's Deployment turns it off, fresh plans; a
  failing fresh plan is replanned.
- **Isaac Sim is attach-only:** you start Isaac Sim and open the scene;
  ManyForge never loads a world or plays, pauses or resets the simulation by
  itself. Replaces 0.1.0's simulator reset with
  `simulation_control.reset.resume_play_after_reset`. See
  [Stack setup](docs/operations/STACK_SETUP.md).
- **A simulation that isn't playing** never blocks Enable, Activate or
  Execute; new motion on it is refused as an operator Stop
  (`simulation_not_playing`), so Program nodes can play the simulation again.
- **Lanes replace 0.1.0's deployment modes** (`composer_only`,
  `commanded_sim`, `real_robot`): each component names its `lane` (`mock`,
  `isaac`, `real`).
- **Real robot lifecycle:** 0.1.0's supervised Lite 6 transport and owner
  liveness rules are replaced by the machine's Prepare and Recover, which
  restart the driver when it is not connected.
- **Transport controls:** **Execute** starts the Program held and each
  motion group has **Run**, **Stop**, **Resume**, **Halt** and **Reset**
  (0.1.0 had one Run/Stop/Reset pill).
- **Manual output writes** are refused only while motion executes or the cell
  is latched; the last write wins.
- **A failed Apply step** no longer stops the others: every affected
  component is still prepared and the result lists each outcome; nothing is
  rolled back.
- **New and Load ask once** when the cell isn't empty, listing what they stop,
  plus the unsaved-Draft warning.
- **Example Scenarios renamed** by lane prefix and scope; artifact ids are
  unchanged.

### Removed

- 0.1.0's native execution backend for simulated lanes: replaced by the Mock
  lane on ROS.
- Pre-dispatch collision sampling **between** a trajectory's waypoints (along
  the executed spline, at the collision-check step): removed with the
  collision clearance; the check now covers the trajectory's own waypoints
  (10 ms grid) and the collision clearance covers the spline's deviation.
  Last implemented on `dev` at commit `0703ea48` (introduced in `53cc751e`; not in 0.1.0):
  `manyforge_core/src/validation_sampling.cpp`,
  `manyforge_core/include/manyforge_core/trajectory/validation_sampling.hpp`,
  and its use in `component_lifecycle_plan_validity.cpp`.
- 0.1.0's runtime status popup (`/api/runtime/status`) and
  `POST /api/runtime/external/{provider_id}`: replaced by `GET /api/components`,
  the `/ws/control` key `component_state` and the Components page.
- 0.1.0's unified launcher (`scripts/launch.sh`, `scripts/demo-*.sh`,
  `scripts/lib/`): replaced by `./scripts/compose-dev.sh` and
  `manyforge-operator`.
- 0.1.0's assistant supervisor probe (`assistant_supervisor.py`) and the
  legacy mux provider: replaced by `ai_model` components and their endpoint
  stream health.
- 0.1.0's topic-based command-gate node: command admission is the Kernel's.
- 0.1.0's `ExecutionKernel::acknowledge_intervention` and the Run-side
  auto-acknowledge: replaced by the derived cell latch.
- 0.1.0's real-robot cell file (`manyforge.real_robot_cell.v1`) and the Lite 6
  supervised commissioning example: replaced by machines and the `real_*`
  Scenarios.
- The loose compiled example Deployments (`examples/*.deployment.yaml`):
  replaced by the Scenario examples.
- The "Universal Robots I/O states" and "Universal Robots set_io" signal
  sources (no model used them; there is no real UR lane).
- `Dockerfile.ros-lyrical-real-xarm` and `docker-compose.realtime.yaml`: one
  Lyrical image serves every lane.
- The leftover Force close machinery (the Kernel's `arm_force_close` and
  `force_close_operations`, the execution host's `force_close` command) and
  its error codes.

### Fixed

- The runtime lookahead no longer stops a move that leaves a contact:
  it compares the plan with itself.
- Component actions are no longer refused when unrelated ROS nodes start or
  stop: only the publisher count of the component's own topics counts.
- An interrupt rule sees every write to a key it reads, in order, before the
  next write or motion: short pulses still trip it.
- A gripper Stop at the end of its stroke no longer races the stock
  controller's cancel.
- A gripper Stop no longer hangs when its cancel arrives just as the goal
  finishes (the stock controller then sends no result): the controller's
  acceptance of the cancel ends the command, and the Isaac gripper bridge now
  holds the fingers before accepting, like the stock controller. A Stop
  also sends no cancel when the controller reported the goal reached or the
  node's `command_duration_s` has passed.
- A rule with `hold` now holds the run when it trips while nothing in its
  scope moves, instead of failing the next motion node.
- xArm: an output write answered `ret=1` counts as accepted; an output shown
  unknown clears when the I/O stream reports it.
- An e-stop held during recovery, or a degraded Stop, no longer wedges the
  lifecycle or silently closes motion admission.
- An Apply that fails before any effect releases the authoring claim.
- An Apply that fails after it started changing the cell no longer locks
  authoring until a restart (`authoring_not_ready`): New (and Load, which
  starts with a New) releases its claim and empties the cell.
- Kernel exit after about 20 Load cycles (Apply journal checked at startup).
- A Stop journaled under a stale owner epoch no longer locks the cell
  ("waiting for durable record").
- A leaked gripper action node and its exit crash.
- Pneumatic finger collision state, a Program cycle Reset race, UR10e Isaac
  Load (`relative_references`), Program editor redraw on every status update.

### Known issues

- Fast DDS 3.6.2 can crash a ROS process with SIGSEGV while it shuts down
  ([eProsima/Fast-DDS #6599](https://github.com/eProsima/Fast-DDS/issues/6599));
  see [Troubleshooting](docs/operations/TROUBLESHOOTING.md#component-process-crashes-on-shutdown).
- rclcpp 32.0.x (Lyrical) can abort a controller process with
  "ExecutorNotifyWaitable::is_ready: Internal error, got index out of range" while it
  shuts down (about 1 in 50 teardowns; no effect on the Load or Stop). Fixed upstream in
  rclcpp 33.1.0 ([ros2/rclcpp #3174](https://github.com/ros2/rclcpp/pull/3174)).
- `time_optimal` Cartesian moves can overshoot the planned TCP speed between
  trajectory samples by up to about 1.4×; `constant_speed` (the default) does
  not. See [RoboPlan adapter notes](docs/reference/ROBOPLAN_ADAPTER_NOTES.md).
- **Many different Loads:** the deployment content stores refuse new content
  after about 1,024 Loads with different content in one Workbench session
  (repeated Loads of the same content don't count). Stop and start the
  Workbench to clear it. Fix planned for 0.2.1.
- **Fast DDS shared memory:** a killed ROS process can leave a stale Fast DDS
  shared-memory port, after which long-running ManyForge processes stop reaching
  whichever component reuses that participant id. The workbench therefore runs
  DDS over UDP (`FASTDDS_BUILTIN_TRANSPORTS=UDPv4`). Under
  `ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST`, rmw_fastrtps forces shared memory
  back on, so prefer the default (`SUBNET`).
- **Rare controller freeze:** in Load soak tests, the stock `ros2_control_node`
  froze about 3 times in 55 teardowns (joint states stop, SIGINT ignored); the
  Load recovered through the controller process kill. Under investigation.
- **Isaac speed:** the arm controllers follow wall-clock time; keep Isaac Sim at
  about real time or moves can abort on path tolerance. See
  [Troubleshooting](docs/operations/TROUBLESHOOTING.md#isaac-slower-than-real-time).

## 0.1.0 – 2026-07-29

First tagged release (`v0.1.0`, `main` at `8ae05265`): the publication-readiness
baseline and the hardening done before the tag. Items that 0.2.0 removes or
replaces are listed in its Removed and Changed sections.

### Added

- **Structured backend fault taxonomy (arm + gripper)**: ros2_control backend
  faults carry a stable `reason_code` (e.g. `tracking_error_exceeded`,
  `gripper_goal_rejected`) with measurements through the execution digest
  (`errorDetails`), with operational severities and externally publishable
  `MANYFORGE_*` fault codes. Gripper faults that previously surfaced as bare
  `tree_failed` prose are now classified end-to-end. A C++/Python parity test
  guards the taxonomy against drift.
- **Standard ROS 2 diagnostics surface**: `manyforge_ros.diagnostics_bridge`
  publishes `diagnostic_msgs/DiagnosticArray` on `/diagnostics` from the
  Composer digest (one status per active fault, taxonomy-mapped, OK heartbeat
  when healthy). Depends only on `diagnostic_msgs` — works with any
  diagnostics consumer.
- **ros2_medkit integration (day 0)**: new `manyforge_medkit_adapter` package
  forwards core faults to an AUTOSAR-DEM-style fault manager (FAILED on
  latch, PASSED healing stream on clear, CRITICAL bypasses debounce, SOVD
  discovery hint), verified against a real `fault_manager` lifecycle.
  Install/test/operate runbook at `docs/operations/ROS2_MEDKIT.md`.
- **Failure archives + event-recorder hardening**: ERROR-class incidents
  persist a freeze-frame archive across reloads; the event recorder writes
  off-lock via an async JSONL writer, rate-limits observation polls, and
  synthesizes semantic execution events across poll-skip windows so terminal
  transitions are never silently dropped.
- **Scheduler priorities**: `IScheduler::schedule(...)` accepts a new
  `SchedulerPriority` argument (default `Safety`, backwards-compatible for
  existing callers). `SimpleScheduler` runs `Safety` and `Monitoring`
  callbacks on independent worker threads so a Safety callback blocked on a
  long-running lock cannot starve a Monitoring watchdog. The planning-timeout
  watchdog in `ExecutionKernel::execute()` is now tagged `Monitoring`. New
  regression test
  `NativeSchedulerTest.MonitoringLaneFiresWhileSafetyLaneIsBlocked` enforces
  non-starvation.
- **Kernel API**: `ExecutionKernel::acknowledge_intervention(principal_id, reason)`
  is the explicit, audited path for clearing `kInterventionRequired`. It emits
  a `kInterventionAcknowledged` event with the supplied principal and reason,
  mirroring the existing `acknowledge_completed` pattern.
- **Cycle-manager Run-side auto-ack**: when the kernel reports
  `InterventionRequired` on Run from `STOPPED` and the leftover state is
  provably execution-local (no active streaming/direct-control lease, no
  running intervention session), the cycle manager automatically
  acknowledges it through the new kernel API. Otherwise
  Run fails closed with a structured reason.
- **Direct-control and streaming observability**: direct-control WebSocket
  paths now have bounded timing checks, structured event draining, rejection
  reasons, and stress coverage for end/new-begin races.
- **Runtime velocity scaling**: in-flight trajectories are retimed when the
  operator changes the velocity scale, instead of stopping and restarting the
  action. Streaming backends also read the runtime scale per dispatch tick.
- **Native Composer viewer and direct jog surfaces**: Composer gained native
  robot/object control affordances, cartesian field jogs through the
  follow-target stream, command-authority gating, and bounded async-store
  behavior tests.
- **Move-To ghost visualization**: unreachable Move-To previews now render
  the full gripper visual subtree where available, rather than relying only on
  the TCP link mesh.
- **Cylinder scene primitive path**: scene authoring and backend surfaces now
  keep cylinder primitives available where RoboPlan supports them.
- **Core kernel** (`manyforge_core`)
  - Bounded-stop / pause / reset state machine with `InterventionRequired`
    terminal state for software-stop failures.
  - Execution timeout enforcement with bounded drain.
  - Collision-monitoring policy with configurable period.
  - Late-callback guards so a stale FJT result cannot overwrite a
    monitor-triggered failure.
  - Resync-after-intervention protocol gated by a fresh observed state.
  - Thread-safe `ManualScheduler` (callbacks fire outside the scheduler lock);
    explicit `ExecutionKernel` worker join before member teardown.
- **RoboPlan adapter** (`manyforge_roboplan`)
  - Native execution backend for simulated lanes.
  - `ros2_control` execution backend with `commanded_sim` and `observed_robot`
    source modes, tracking-error monitor, peak-tracking-error telemetry, and
    `resync_required` recovery.
  - Direct use of upstream RoboPlan `Scene::addMeshGeometry` API (the LIFO
    workaround for missing-upstream-API is retired).
  - Support-contact collision policy with per-query allowance (no global
    scene-mutating sweeps in the hot path).
- **Composer** (`manyforge_composer`)
  - FastAPI backend + React/ReactFlow frontend for behavior-tree authoring.
  - Three deployment modes: `composer_only`, `commanded_sim`, `real_robot`
    (detected from deployment YAML).
  - Runtime status surface (`/api/runtime/status`) with the profile-aware
    popup pill + per-component rows.
  - External-supervisor registration endpoint
    (`POST /api/runtime/external/{provider_id}`) so launchers can feed
    additional component status into the popup.
  - File-backed failure archives with age + byte-budget FIFO cleanup.
  - Heartbeat endpoint (`/api/infra/heartbeat`) for external watchdogs.
  - AssistantOverlay gated on `deployment.assistant.enabled`.
- **Assistant modes + bridge** (`manyforge_assistant_bridge`)
  - Bounded-autonomy bridge serving the assistant-provider HTTP contract
    against an OpenAI-compatible upstream (vLLM).
  - Two production lanes: `nemoclaw` (direct vLLM, port 8100) and `openclaw`
    (sandboxed OpenClaw gateway, port 8200 - production default since
    2026-05-07).
  - Mode-scoped MCP wrapper enforcing per-mode tool catalog.
  - Periodic `/compact` neutralization of chain-session cascades (iter-32
    production recipe).
  - New `assistant_supervisor.py` long-running probe poster that feeds
    Composer's runtime-status surface with vLLM / proxy / OpenClaw / bridge
    readiness rows.
- **Behavior + Composer viewer** (`manyforge_behavior`, `manyforge_composer`)
  - py_trees-based behavior-tree runner with `CycleManager`
    (run/pause/stop/reset, intervention sessions).
  - Native 3D scene visualization and control-panel surfaces.
- **ROS bringup** (`manyforge_ros`, `manyforge_ros_bringup`)
  - Order-tolerant `isaac_demo_supervisor` ROS node that loads the Composer
    program/deployment once Composer + Isaac + arm/gripper actions are all
    reachable, then triggers a home reset through the normal execution
    backend.
  - Isaac gripper action bridge translating `control_msgs/GripperCommand` into
    Isaac's raw `JointState` command topic.
  - Topic-based command-gate node (fail-closed by default).
- **Unified launcher** (`scripts/launch.sh`)
  - NemoClaw-style 7-step wizard with review gate.
  - Lane (manyforge-only | isaac-sim) x assistant (on | off) selection via
    flags or interactive prompts.
  - Nine named scenarios including Isaac+AI
    (`isaac-ur10e-light-scene-assistant`) and the
    `ur5-simple-pick-place` 3-step test fixture. Old short names
    (`pick-place`, `ur10e`, `shape-pick`, `vision-pose`,
    `assistant-authoring`, `isaac-light`, `isaac-assistant`) still resolve via
    an alias table with a one-time deprecation warning per launch.
  - Concern-organised lib modules under `scripts/lib/`.
  - Seven `scripts/demo-*.sh` collapsed to thin shims.
  - Idempotent re-runs against partial state via lane-aware
    `expected_port_owner`.

### Changed

- **Real-robot candidate and observed-state authority**: approved Robot Control
  candidates now carry the same TCP and per-joint dynamics caps through
  preview, approval recheck, and execution. ROS-backed physical execution
  requires a fresh measured endpoint before accepting controller success, and
  terminal/idle scene projection continues following measured robot state with
  truthful source age while preserving execution metadata. The catalog is
  `0.5.0`; in-tree programs were migrated to that exact version.
- **Real-Lite6 lifecycle progress remains observable during bounded vendor
  operations**: lifecycle primitives and command-gate renewals now run on
  separate serialized NRT workers, provider status/authority/mutation
  callbacks use independent groups, and Composer status reads no longer wait
  behind authority-session transport I/O. Late or post-deadline primitive
  completions are audited without mutating a replaced transaction.
- **Real-Lite6 owner liveness is independent from robot telemetry
  freshness**: a transient stale robot report still fences motion immediately
  but no longer misclassifies an advancing, identity-valid ROS owner as dead
  or revokes its authority session. Owner-publication delivery uses the
  generated cell heartbeat budget. Robot-observation delivery separately uses
  the generated critical-progress budget, capped by the producer's 1000 ms
  observation lease, so ordinary 400 ms xArm report gaps do not become false
  re-arm events. Generic admission publication retains its independent 250 ms
  lease, while the internal xArm write gate now follows the producer's 1000 ms
  observation lease; neither can outlive fresh robot state. Stalled owner
  publication, genuinely stale robot observations, and expired
  lifecycle-command deadlines remain fail-closed.
- **Real-Lite6 supervised transport loss is terminal and fail-closed**:
  generated deployments retain the 250 ms command-admission lease and add a
  2000 ms report/control-loss timeout. Prolonged stale reports or failed
  control reads require a fresh controller-manager process; reconnecting a
  cable cannot silently reuse or re-arm the stale SDK session.
- **Composer editor chrome restructured (industrial-HMI pass)**: the
  full-width bottom bar is replaced by a slim top header (program name,
  passive applied/saved status dots, Save/Save As/Load as uniform icon
  buttons with instant tooltips, Discard/Revert/Apply with Apply as the
  single accent), a floating view-options pill on the graph canvas, and a
  pendant-style transport pill (Run/Stop/Reset + velocity) docked
  bottom-center of the 3D viewer — width-stable across state changes, color
  reserved for state (Run green only when armable, Stop the only red
  control, Reset amber when it would discard live cycle state). Node
  palette drag previews are node-sized, link handles are larger, node
  category colors are richer, and viewer side panels scroll instead of
  covering the transport controls.
- **Launcher forwards frontend dev-build env vars**
  (`MANYFORGE_FRONTEND_SOURCE_MODE`, `MANYFORGE_FRONTEND_FORCE_REBUILD`)
  into both lane containers — previously a worktree dev build requested at
  the launcher silently fell back to committed HEAD inside the container.
- **Native viewer is the sole render path**: `hmiAvailable`/`hmi_available`
  renamed to `viewerAvailable` across Composer models, routes, and frontend;
  the `hmi_snapshot` node image source is now `viewer_snapshot` (node catalog
  `0.2.0`). The VLM snapshot broker fails closed when no browser client is
  connected.
- **Simulator reset resumes play by default**: for commanded-sim deployments
  the reset flow re-asserts `playing` after readiness and reopens command
  authority only after play is restored (`simulation_control.reset.
  resume_play_after_reset`, default `true`; explicit opt-out for deployments
  that deliberately operate paused). Fixes the paused-simulator lockout after
  a looping program's trailing reset.
- **`before_planning` pose-source latch widened**: every enabled detached
  object bound with the policy is refreshed at each planning latch (not only
  objects referenced by the move being planned), because planning
  collision-checks the whole scene; missing or stale observations fail the
  latch closed instead of planning against a phantom pose.
- **Legacy mux/plugin provider quarantined**: `configure-manyforge-provider.sh`
  and related mux paths refuse to run unless
  `MANYFORGE_ENABLE_LEGACY_MUX_PROVIDER=true`; the assistant-provider bridge
  lanes are the only default path.
- **Hot-path performance hardening**: the ros2_control execution monitor uses
  condition-variable waits instead of polling sleeps (sub-10 ms stop/cancel
  reaction), pybind execution surfaces release the GIL, and hot-path
  diagnostics are structured and rate-limited.
- Clarified contributor and LLM-agent guidance: ManyForge implementation work
  is spec-driven by the maintainer's private specifications, and development
  without them is limited to bounded fixes/proposals until maintainer audit.
- **RoboPlan scene-lock contention narrowed**: the speed-validation loop in
  `RoboPlanPlanningContext::validate_trajectory` now acquires the scene mutex
  once across all waypoints instead of per-iteration. Full FK/IK parallelism
  with planning remains blocked on RoboPlan 0.3.0 exposing a thread-local
  Pinocchio `Data` view.
- `RecoveryTrigger` enum aligned with the canonical taxonomy: `EXECUTE_TIMEOUT` and `POSTCONDITION_FAILED` added. The legacy
  `TIMEOUT`/`NODE_FAILURE` aliases were subsequently removed entirely (see
  Removed); persisted traces carry trigger strings outward only and are never
  parsed back into the enum.
- Cycle-manager state-machine documentation now shows
  the real five-state model (`IDLE/RUNNING/STOPPED/INTERRUPT_HANDLER/`
  `DIRECT_CONTROL`) instead of the legacy three-state diagram.
- `manyforge_ros_bringup`: license declaration changed from `Proprietary` to
  `Apache-2.0`; maintainer email updated.
- Composer monitor popup (`RuntimeStatusOverlay`) is now visible on every
  launcher invocation; rows adapt to the profile detected from the deployment
  YAML.

### Removed

- **Legacy Viser/HMI stack**: `manyforge_hmi/`, `ViserPanel`, the `viser`
  dependency, launcher/compose wiring, and the HMI test lane are deleted; the
  native Composer viewer owns the operator and assistant-visible render
  surface.
- **`SkillDefinition.expands_to` compatibility alias** removed from live
  REST/MCP assistant surfaces (canonical `body`/`expandsTo` contract only).
- **`RecoveryTrigger.TIMEOUT` / `RecoveryTrigger.NODE_FAILURE`** deprecated
  aliases removed; the parser rejects the legacy strings.
- **MCP intervention surface**: `manyforge_intervention` execute action no
  longer accepts `trigger_reason="manual_request"`. The MCP allowlist now
  mirrors `RecoveryTrigger.value` exactly; `manual_request` is a
  `session_kind` concept (carried automatically by `manual_intervention`
  sessions) and not a recovery trigger. **Migration**:
  external MCP clients that explicitly passed `trigger_reason="manual_request"`
  should omit the field (the default for `manual_intervention` is unchanged)
  or pass a canonical recovery-trigger value when starting a
  `delegated_recovery` session.

### Contract changes before first tag

- **Cycle Stop now halts preparation-phase executions**: `cycle.stop()`
  previously paused the kernel only when it was already `EXECUTING`; a stop
  landing during `PLANNING`/`VALIDATING` left the async worker free to
  dispatch the motion after the cycle had reported stopped (observed live:
  the arm moved for ~11 s post-Stop). Preparation-phase executions are now
  stopped outright (Run-from-STOPPED replans from fresh state, so nothing
  is lost) and the preparation→executing race is closed with a bounded
  re-check.
- `TrajectoryCache` revision-less `store`/`lookup` overloads are removed:
  every stored trajectory must declare the collision-world revision it was
  validated against; planning/execution hits use `lookup_with_revision`,
  existence probes use `contains`, and the explicitly named
  `peek_unvalidated` serves observability/debug reads only.
- Collision-validation rejections no longer pay a duplicate legacy
  collision-compare query: the `legacy_collision_compare_*` diagnostic keys
  are gated behind `ExecutionKernelConfig::
  enable_legacy_collision_compare_diagnostics` (default off).
- The execution digest promotes classified `reason_code` tokens (including
  the new gripper classes) to the top-level `errorCode`, and external
  diagnostics surfaces now latch faults from any non-`running` digest state —
  tree-terminal failures resting in `idle` are no longer invisible to
  `/diagnostics` and the medkit adapter.
- `ExecutionKernel::execute()` now rejects with `kErrorInvalidRequest`
  (details: `reason_code=intervention_required`) while the kernel is in
  `kInterventionRequired`. The previous implicit clearing of intervention by
  re-issuing `execute()` is removed to preserve the operator-intervention
  audit boundary. Callers must now call
  `acknowledge_intervention(principal_id, reason)` before re-running. Cycle
  manager and Composer Run route already do this transparently when the
  leftover state is execution-local.

### Tests and qualification

- Added C++ lifecycle, streaming, scheduler, ROS soft-stop, and soak bounds
  regression coverage.
- Added Composer frontend tests for command authority, async-store stale
  response handling, direct-control observability, and bounded polling surfaces.
- Added backend tests for URDF/gripper visualization routes and Composer
  dependency coverage.

### Repository hygiene

- Added LICENSE (Apache 2.0) + NOTICE (third-party attribution).
- Added top-level VERSION file as the version source of truth.
- Synchronised version `0.1.0` across all declaration sites
  (`CMakeLists.txt`, `pyproject.toml`, `package.xml`, `setup.py`,
  `package.json`, frontend lockfile).
- Added CHANGELOG.md, ROADMAP.md, CONTRIBUTING.md, SECURITY.md.
- Added GitHub PR + issue templates.
- Added CI workflows: Python tests, frontend build, bash syntax, private-spec
  boundary, version sync (in addition to the existing cpp-tsan lane).
- Composer package metadata now declares the URDF/mesh parsing dependencies
  used by the native viewer and gripper-limit validation path.
- AGENTS.md and README updated with the new `dev`-branch workflow and a
  "For LLMs" entry point pointer.
