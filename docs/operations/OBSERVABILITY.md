# Observability — Logs, Endpoints, Diagnostic Capture

Where ManyForge's logs go, which REST endpoints report which state, and what
to capture for a bug report.

For symptom-to-cause mapping see
[`TROUBLESHOOTING.md`](TROUBLESHOOTING.md). For the full route reference,
see [`../reference/REST_API.md`](../reference/REST_API.md) (generated from the
FastAPI app).

## Logs

ManyForge does **not** ship a unified log file. All workbench processes write
to the workbench container's console, captured by Docker; runtime and alarm
events go to the Historian and are read through REST; the AI assistant
component's bridge also writes a JSONL audit file.

### Workbench console

- **Where**: the workbench container's console. Child-process output
  (Composer, Headless API, Program runner, ROS host) is inherited by it:
  `docker logs --timestamps --tail 200 manyforge-development-workbench` (or
  the `MANYFORGE_DEV_CONTAINER_NAME` you set); `--timestamps` prefixes each
  line with the UTC time Docker received it, which matches History's UTC
  times. Docker rotates it at 50 MB × 3 files. `./scripts/compose-dev.sh stop`
  keeps the last 10 MB, with those times, under the runtime root's `evidence/`
  (see [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md)).
- **Python process format** (Composer, Headless API, Program runner): one
  JSON object per line on stderr, from
  `manyforge_headless_api/logging_setup.py`, with `timestamp` (UTC, ISO-8601
  with microseconds and a trailing `Z`), `mono_ns` (CLOCK_MONOTONIC),
  `level`, `process` (`composer`, `headless_api` or `program_runner`), `pid`,
  `logger` and `event` (the message), plus `exception` (the traceback) and
  any call-site fields; a call-site field that collides with one of these
  appears as `extra_<name>`. `uvicorn.access`, `httpx` and `httpcore` are
  quieted to WARNING. Each process takes `--log-level` (`DEBUG`, `INFO`,
  `WARNING`, `ERROR`; for the Headless API on `serve`): Composer defaults to
  INFO and adds one line per request (path, status, duration_ms, client);
  the Headless API and the Program runner default to INFO. Uvicorn's own
  start and error lines keep uvicorn's text format.
- **Fatal signals**: on SIGSEGV, SIGFPE, SIGABRT, SIGBUS or SIGILL,
  Composer, the Headless API and the Program runner write every thread's
  Python traceback to stderr (`Fatal Python error: …`), then end as before.
- **Component actions**: Prepare, Enable, Disable, Recover and the other
  component actions end `succeeded`, `failed` with a typed error (and the
  vendor's code and text when the controller gave one), or `unknown` when
  their effect could not be confirmed; nothing resends an `unknown` action.
  The latest result is the component's `last_action` in `GET /api/components`.
  What comes next is decided from the component's current state.

### Application end and fault lines

These lines go to the same console (the workbench) or journald (the
operator launcher's `standard` profile):

- `<process> exited with status N` (or `with signal N`), from the Manager,
  where `<process>` is `headless`, `program_runner`, `composer` or
  `execution_host`: that process exited, so the Manager ended the whole
  application. Read the lines before it for the cause.
- `{"code":"manager_planned_stop_overrun","outcome":"…"}` and
  `{"code":"execution_host_planned_stop_overrun","outcome":"…"}`: while the
  application was ending, the Manager's or the execution host's Halt of
  the cell was refused, could not be made, or its state could not be read
  (`outcome` says which), and shutdown continued. Check that the robot has
  stopped.
- `application stop: waiting at <step>: <what>`, then
  `application stop: <step> complete`, `application stop: <process> exited
  pid=N status=N` (or `signal=N`) or `application stop: manager_records
  written`: the Manager's stop, step by step (`halt`, each application
  process, `manager_records`). The execution host's steps show as
  `native shutdown entering/waiting at <phase> …`, starting with
  `planned-halt`. No step has a time limit; the last such line names where a
  stop that does not end is waiting.
- `{"code":"application_stop_forced","step":"…"}`: a second stop request
  came while the stop was waiting at that step; the processes still running
  were ended. Each one gets
  `{"code":"application_process_killed","role":"…","pid":N}`. The Manager
  then ends with status 74 (a stop that did not complete; also when the
  execution host's exit was not clean or the Halt did not complete), and
  the next start reports the shutdown as unclean or not verified.
- `manyforge_journal_drain queued=N`: the execution host's operation journal
  starts writing its last N records at shutdown. Records it could not write
  are counted afterwards in `operation_journal_records_lost`.
- `Manager liveness packet refused: <cause>`: the execution host refused a
  malformed or unexpected message from the Manager and latched its internal
  fault (`manager_liveness_packet_refused`); motion stays blocked until the
  application is started again.
- `{"code":"execution_host_late_reply","exchange":"…","waited_ms":…}`:
  a Manager request to the execution host (`exchange`) was still unanswered
  at the Manager's next tick, `waited_ms` after it was sent. Information
  only: nothing is halted, the host keeps running and does not count as
  faulted; meanwhile other requests to it answer `execution_host_late`. Its
  answer is logged as `{"code":"execution_host_late_reply_settled","exchange":"…"}`.
  A long Host Apply logs it as a matter of course; a status or liveness
  request that stays unanswered points to a stalled host: keep the log and
  report it.
- `execution host start-up: …` (`pid=… runtime_unit=…`, `command surface
  ready`, `serving the Manager channel`), then the Manager's `execution host
  launched: pid=…; waiting for its readiness` and `execution host ready:
  pid=…`: the host's start-up steps; the launch has no time limit, so the
  last line names where it waits.
- `kernel rpc timing: command=… pid=… elapsed_us=…`: one Manager request to
  the host that took more than 200 ms (at most one line per second per
  request kind).
- `native projection preparation started: pid=… binding=…`, then
  `native projection preparation answered: … elapsed_ms=…` (or `ended
  without its reply`): one projection preparation, which has no time limit.
- `execution host exchanges abandoned: the application is stopping`: the
  stop ended the host requests that were waiting for their answers; a
  reply that arrives later is logged as `abandoned exchange reply settled:
  pid=… exchange=…` (`abandoned liveness reply settled: …` on the liveness
  channel).
- `manyforge_fault code=<code> …`: an internal fault or stall, named by its
  code. Keep the log and report it.

#### How the application ended

When the application ends because a process exited, a stop signal came,
`shutdown_runtime` was requested or the Manager failed (an internal error, or
a failed start of the processes that start after the first baseline), the
Manager keeps that first event in memory and, once its stop has ended, writes
it once to `current/manager-run/application-end.json`
(`manyforge.application_end.v1`): `ended_by` (`process_exit`, `stop_signal`,
`shutdown_runtime` or `manager_failure`), `process` (for `process_exit`: its
`role`, `pid`, `exit_status` or `signal`, and `core_dumped`; otherwise null)
and `ended_at_wall` (UTC). A Manager that fails while it starts up, or is
killed, writes no file; a failed write is logged as
`application end record not written: …`. After the Manager exits, the
launcher copies the file into the run's `session.json`
(`manyforge.workbench_session.v3`, field `application_end`) together with the
Manager's own exit (`manager_exit`: `exit_status` or `signal`). The next
start's previous-run report (`manyforge.workbench_previous_session.v4`)
carries it as `application_end`, null when the run has none, together with
`started_at_wall` (the previous run's start in UTC, null when its
`session.json` could not be read) and `reported_at_wall` (when the launcher
wrote the report, UTC). History files the report under the previous run's
start; with `started_at_wall` null its time is shown as unknown. A session of
an older build, archived at an upgrade, has an unknown shutdown; its report
names the older marker format in `archived_older_marker` (null otherwise), and
the unclean-session alarm says it came from an older build and names its
archive (`previous-runs/<id>`).

#### Crash output

Core dumps are off by default: every process the launchers and the Manager
start (the Manager, the execution host and its component processes, Headless,
the Program runner, Composer) runs with the soft core-file size limit at 0.
The site settings member `core_dumps` (`{"enabled": true, "max_bytes": …}`,
see [Site settings](DEVELOPMENT_WORKBENCH.md#site-settings)) turns them on:
the soft limit is then `max_bytes`, at most the hard limit, which ManyForge
never raises. Where a core goes and how long it is kept are decided by the
host's `kernel.core_pattern` and its crash handler, not by ManyForge, which
changes neither: with a file name or path the kernel writes the core there
(a relative path is relative to the crashed process's working directory,
inside the container for the workbench); with a pipe to a crash handler
(`|…`, such as systemd-coredump or apport) the kernel passes the dump to that
program, and what it keeps follows that program's own configuration.
`max_bytes` bounds each core; ManyForge does not bound how many are kept.
Headless, the Program runner and Composer start with `-X faulthandler`: on a
fatal signal they print the Python traceback of every thread to their output
(the console or journald), then end as before.

### Assistant bridge audit

The AI assistant component's bridge writes its audit JSONL to
`<execution run root>/controller-launch-artifacts/<component_id>/assistant-audit.jsonl`;
in the workbench that is
`/tmp/manyforge-state/current/execution-run/controller-launch-artifacts/<component_id>/assistant-audit.jsonl`
inside the container, which is
`.manyforge-dev/runtime/<container-name>/current/execution-run/controller-launch-artifacts/<component_id>/assistant-audit.jsonl`
on the host. It is rotated at 10 MiB, with the active
file plus 3 backups kept. It records every assistant request and tool call
with identities, hashes and outcomes (never prompts, responses or argument
values), including fail-closed rejections and upstream errors. On the next start, `current/` moves to `previous-runs/`.

### Runtime events

Runtime and alarm events are written asynchronously to the bounded Headless
Historian. Composer does not buffer, merge, or persist a second event stream.
The public read surface is:

- `GET /api/execution/history` — one bounded 32-row summary page;
- `GET /api/execution/history/{sequence}` — exact detail for one selected row;
- `GET /api/execution/history/health` — Historian writer health; and
- `GET /api/execution/alarms` — current condition-backed alarms.

The browser mounts only the current summary page and requests detail on
selection.

History never holds up the cell. A record it cannot take (its queue of 1024
is full) or cannot write is dropped and counted, logged once per loss
episode, and motion is not affected. While records are being lost, the
History health reports `available: false` and the alarm "History is missing
records" is active; both clear by themselves once History has written its
queue empty again. If the alarm stays, check the disk.

### Collision-check timing

Each robot checks for collisions once per check period while it moves. A
check that ends one or more whole periods after it was due is late; the
periods it overran are skipped, not caught up. The first late check of a run
of late checks writes one `manyforge_fault code=runtime_collision_checks_late`
line to the log. `GET /api/execution/diagnostics` (MCP `manyforge_kernel`,
action `diagnostics`) shows the counts per robot in
`status.runtime_collision_checks`, one row per robot: its
`controllable_component_id`, `checks_run`, `checks_late`, `periods_skipped`
and `check_period_ns`. They are totals since the robot's motion
runtime in the execution host was last built.

### Program tick timing

The Program runner ticks each run's tree once per tick period (10 ms by
default). A tick that ends after its period is late; it is counted, and
nothing is stopped or failed for it. A run of consecutive late ticks of one
Program run writes one `manyforge_fault code=program_ticks_late` line when it
ends (at the first tick on time again, or when the run stops being ticked),
and one more every 1000 late ticks while it lasts. The line names the
`program_run_id` and `period_ns`, and gives `late_ticks` and `longest_tick_ns`
of this run of late ticks, the run's totals (`late_ticks_total`,
`longest_tick_ns_total`, `episodes_total`) and `line`/`max_lines`: one
Program run writes at most 64 such lines, after which its late ticks are only
counted.

The Program runner's full memory clean-up, which stops every runner thread,
runs at start-up, Load and Reset; there and when a run starts it sets the
long-lived objects aside so that the automatic full clean-up during a run has
little to scan. When automatic full clean-ups still ran since the previous of
these points, the point writes one `program_runner_heap_settled` line with
`point`, `automatic_full_collections` and
`longest_automatic_full_collection_ms` since the previous point,
`collection_ms` (the point's own pause; `0.000` at `run_start`, which does not
clean up) and, except at `run_start`, `frozen_objects`.

The History file is named by its event format, `history.v<N>.sqlite3`
(currently `history.v10.sqlite3`), in the History directory (`history/` under
the development state root and in an operator instance). Only the file of the
current format is opened. An earlier History file
(`history.sqlite3`, or a `history.v<N>.sqlite3` with a lower number) stays on
disk unopened and unconverted, and the History panel's retention notes say
which file holds that earlier history. Older History files count toward the
2 GiB kept for previous runs and are deleted oldest first; the current file
never is. Within the file, routine events, important events (errors, Stops,
acknowledgements) and Program node attempts (`program_node_attempt_finished`)
are pruned separately, oldest first, each class above 100000 events or its
event bytes (16 MiB routine, 16 MiB important, 32 MiB node attempts), so a
long run's node attempts never prune the Applies, Enables and moves before
them.

History keeps the events of earlier application starts, and every event
belongs to one start: `application_start_id` (16 hex characters) is a filter,
and each row of a History page names its start. Operation, Stop, run and node
attempt numbers start from 1 again in every start, so a filter by one of them
(`operation_id`, `stop_event_id`, `program_run_id`, `node_attempt_id`,
`parent_node_attempt_id`) or by `action_span_id` means the current start unless
another start is named: add `application_start_id=<16 hex>`, or give the id in
its full form `<16 hex>/op7`. The display form as Composer shows it, for
example `operation_id=3f9a/op7`, names its start only by four characters, so
it must be the named start's, or the current start's when none is named;
otherwise the query is refused with `historian_query_invalid`.
`application_start_id` without a number lists all retained events of that
start. Time, component, type, search and alarm filters without it span every
retained start. The list of retained Program runs
(`GET /api/execution/history/discovery?kind=program_run_id`) covers the current
start, or the start named by `application_start_id`, in number order;
`kind=application_start_id` lists the starts that have retained events, newest
first. In Composer's History panel, the Start filter chooses the current start
or an earlier one.

The previous-session report and the "previous session unclean" alarm describe
the previous start: they are filed under that start, so `operation_id=7` with
the previous start's `application_start_id` shows that operation's own events
and the report that lists it as unfinished.

History also records what was asked of the components and the cell, once per
terminal, with the typed error code and message:

- `component_action_finished` — one per component action (Prepare, Enable,
  Disable, Recover, Reset position, Power off, Reboot, Sim reset), whoever
  asked for it (a button, Enable all, an Apply step). The event's component is
  the component id; the payload has `action`, `outcome` (`succeeded`,
  `failed`, `unknown`, `refused` when it was never started, `interrupted` by a
  Stop) and `error` (`code`, `message`, and the vendor's code and text when the
  controller gave them). Severity: INFO on success, WARN when refused or
  interrupted, ERROR when failed or unknown.
- `apply_finished` — one per Scenario Load and New, Deployment Apply and
  Revert, and Program Apply and Revert (component `manyforge.composer`). The
  payload has `subject` (`scenario_load`, `scenario_new`, `deployment_apply`,
  `deployment_revert`, `program_apply`, `program_revert`), `outcome`
  (`succeeded`, `failed`, `refused` before any effect, or `accepted` when a
  Program Apply was handed to the Manager), the first `error`, and the Apply's
  component rows, each with its own `error`. A Load records one event; its
  emptying New is part of it.
- `program_reset_finished` — one per Program Reset, at its end (component
  `manyforge.program_runner`); a Reset continued by a later confirmation
  records again at that confirmation's end. The payload has `outcome`
  (`succeeded`, `refused` before it was accepted, `failed`, `unknown` when its
  effect needs reconciliation), `error`, and `reset`: the options
  `reload_initial_scene`, `reset_robot_position` and `reset_stored_values`,
  the `program_run_id` it reset (null without a run), `cause_code` of a
  failure, and, when this Reset reloaded the scene, the Applied Program it
  used (`program_revision`, `applied_record_digest`) and the logical-scene
  revision its verified readback left (`scene_revision_id`,
  `scene_content_hash`). The line reads
  like `Program reset succeeded · reloaded initial scene (scene revision 10)`.
- `program_node_attempt_finished` — one per finished Program node attempt,
  sent by the Program runner (component `manyforge.program_runner`). The
  event's time is the attempt's end on the runner's clocks (`clock_owner`
  `program_runner:<start>`), not the time History received it. The
  payload has `subject` (the node type, for example `motion.move_to_pose`),
  `outcome` (`succeeded`, `failed`, `interrupted`, `stopped`, `preempted`),
  `error` (only when `failed` or `interrupted`: `code`, `message` and the
  node's bounded `details` when it has them), and `attempt`: the
  `program_run_id`, `node_instance_id`, `node_attempt_id`, `started_at_wall`,
  `finished_at_ns`, `finished_at_wall`, `duration_ms` and, for a scene node,
  `scene_operation` and `scene_resource_id` (null otherwise). The runner
  sends these events in batches; any it could not deliver are counted in
  `node_attempts.lost_count` of `GET /api/program/diagnostics/health`. Severity: WARN when failed, otherwise
  INFO; a failed condition node is ordinary control flow. Filter a run's node
  timeline with `program_run_id=<n>` (or `node_instance_id`,
  `node_attempt_id`). The line reads like
  `Node pick_1 (motion.move_to_pose) failed: <code>: <message> · run 3 attempt 12 · 1.24 s`,
  with `· add object box_1` for a scene node.
- `scene_change_finished` — one per scene change made outside a Program node
  through `POST /api/scene/transactions` (component `manyforge.scene`):
  Composer, the assistant, MCP, any HTTP caller, and the Program runner's own
  changes (initial-scene preparation, a Program Reset's scene reload, the
  scene clear of an empty Apply; `source_label`
  `manyforge.behavior.program_scene` for the runner's, which commits them at
  the Manager and records them through
  `POST /api/execution/history/scene-changes`). A Program node's
  scene change is in its `program_node_attempt_finished` event, and a request
  sent again that is answered with its first result records nothing again. The payload has `subject` (the operation:
  `add_object`, `upsert_object`, `remove_object`, `update_object_pose`,
  `attach_carried_object`, `detach_carried_object`,
  `detach_carried_object_at_current_pose`, `set_collision_policy`),
  `outcome` (`succeeded`, `refused` by the scene owner, `unknown` when the
  owner could not tell whether its view of the scene changed, `failed` when
  Headless could not complete the request, for example when the owner did not
  answer or an added object's mesh could not be read), `error` (with the
  owner's `details` when they fit 2 KiB), and
  `scene_change`: `scene_resource_id` (null for an operation without an
  object), `source_label` and `principal_id` of the caller, `duration_ms` in
  Headless until the owner's answer, and `scene_revision_id` committed (null
  unless succeeded). Severity: INFO on success, WARN when refused, ERROR when
  failed or unknown. The line reads like
  `Scene add object box_1 succeeded · composer_ui · 12 ms`.

- `command_finished` — one per operator command at its answer (component
  `manyforge.headless`): Execute, motion start and stop, resume, resume or
  discard a halt, blackboard write, manual result and handover, operation
  start, reset and acknowledge, recovery action and persistent values, each
  accepted or refused, and runtime scale changes; for Stop, Stop all, Halt
  and alarm acknowledgement only refusals (an accepted Stop has its
  `stop_event_*` events). The payload has `subject` (the command), `outcome` (`accepted`,
  `succeeded`, `refused`, `failed`, `unknown`), `error` (with its `details`
  when they fit) and `command` (the command kind, run or operation number,
  `scale` for runtime scale, and `repeat_count`). An identical refusal within
  10 s of the last row written for it is only counted, and so is an accepted
  runtime scale change from the same source and transport (a slider sends one
  per step); the next row written for it carries the count and, for runtime
  scale, the latest scale.
  Severity: INFO, WARN when not accepted.
- `io_output_written` — one per manual output write (component
  `manyforge.io`): `subject` is the output, `io_output` has the `value`, its
  `origin` and `duration_ms`. Program writes are in the node attempt.
- `motion_trace_captured` — one per automatic motion-trace capture (component
  `manyforge.motion`), when a trace store is set (`MANYFORGE_MOTION_TRACE_DIR`):
  after an operation with a robot motion ends `failed`, `interrupted` or
  `intervention_required`, or after a robot's Stop escalated, Headless has the
  execution host write the commanded-vs-measured trace it holds in memory
  (the last 16 goals and controller samples) to the store, once per operation,
  one capture at a time; requests for a robot that already has one waiting
  are counted into it. `subject` is the robot, `outcome` `succeeded` or
  `failed` (with the `error`, for example `motion_trace_store_full`), and
  `motion_trace` has `cause` (`operation_failed`, `stop_escalated`),
  `operation_id`, `operation_revision`, `operation_state`, `stop_event_id`,
  `program_run_id`, the
  `file` name in the store, its `bytes`, `status` (`complete`, or `partial`
  when the window had already dropped data) and `request_count`. The capture
  never delays motion, Stop or other commands: the file is written by its
  own host thread and nothing is retried. Severity: INFO, WARN when failed.
  The line reads like `Motion trace of robot_1 saved: trace-<sha256>.json · operation 12 failed`.
- `program_run_started`, `program_run_finished` and the runner's audit
  events (`cycle_recovery_action_applied`, `program_operator_choice_answered`,
  `program_node_automatic_retry`, `program_node_preempted`,
  `persistent_value_written` and the others the run's audit list shows), sent
  by the Program runner like node attempts and stamped when they happened on
  the runner's clocks. The payload has `subject`, `outcome` (`started`; the
  end state `succeeded`, `failed`, `stopped`; `recorded` for an audit event),
  `error` when failed, and `run_event`: `program_run_id`, `occurred_at_ns`,
  `occurred_at_wall` and the event's facts. Severity: INFO, WARN when the run
  failed or stopped.
- `historian_records_lost` — one WARN row when a run of records that History
  could not take ends, with `lost_count`, `reason_code` and the first and last
  lost time.
- `clock_anchor` — one row per start: Headless's event clock
  (`owner_monotonic_ns`, `clock_owner` `headless:<start>`) next to the host's
  `raw_monotonic_ns` and `utc_wall`, read back to back, so Headless times can
  be ordered against the runner's and the Kernel's raw monotonic times.

A record made while serving a request may carry `caller`: `request_id` (the
`X-Request-ID` Composer forwards), `request_key`, `transport` (`http`, `ws`,
or `mcp` for a request made through Composer's MCP adapter), `source_label`
and `principal_id`. A runtime-collision event's line names the bodies, like
`... (lite6_link5 <-> fixture_box)`.

In Composer's History panel row times show the UTC date and milliseconds; a
selected row shows the recorded event as JSON with Copy, and its ids as
"Set filter" buttons; Follow reads the newest page every 2 s while the panel
is visible; a page filtered to one Program run also shows the run's timeline,
oldest first.

The list line reads like `Enable lite6_robot failed: <code>: <message>`; select
the row for the full payload. Filter with `event_types=component_action_finished`
or `components=<component id>`. These events share the History's existing
retention limits.

## Observability REST endpoints

All paths are HTTP routes on the Composer API, `http://127.0.0.1:9020` in the
workbench. The full route list with parameters lives in
[`../reference/REST_API.md`](../reference/REST_API.md). Below is the
operator-facing subset.

### Health / readiness

| Endpoint | Use it when |
| --- | --- |
| `GET /api/health` | First check after starting: is Composer alive? |
| `GET /api/runtime` | Runtime owner availability as seen by Composer. |
| `GET /api/components` | Every component's state (lifecycle, fault, reason, streams, last action, controller details), as on Composer's Components page. |
| `GET /api/cell/latch` | Whether the cell is latched, and the components that cause it. |
| `GET /api/program/readiness` | Whether Execute and Resume are allowed, with the blocked reasons. |
| `GET /api/scenarios/current` | The active Scenario and its identity. |

`./scripts/compose-dev.sh status` combines the owner-issued current Scenario
with `ready` and `unavailable_reasons`; it is the default check after `start`.

### Execution state

| Endpoint | Use it when |
| --- | --- |
| `GET /api/cycle/status` | Cycle state and the focused versioned `recovery_offer`. Every held node's offer (`recovery_offers`) and every open operator question (`operator_questions`) are in the run's runtime controls (`GET /api/program/runs/{id}/runtime-controls`). |
| `POST /api/cycle/recovery-actions` | Apply one exact action from a current offer, or answer an open operator question (its `offer_id` and `offer_revision`, the option id as `action_id`). Do not replay a cached offer after a conflict. |
| `GET /api/execution/history?...` | Filtered event page. Filters include `event_types`, `minimum_severity`, `components`, `program_run_id`, `operation_id`, `recent_seconds`, `search`; an id filter needs the start identity (see [Runtime events](#runtime-events)); `limit` is at most 32, page with `before_sequence`. `search` is a case-insensitive substring (at least 3 characters) of the component, the event type, the message and reason codes, a component incident's cause, closure and vendor codes, the alarm subject, and the event's run/operation/Stop/incident/condition ids; other payload fields are not searched. |
| `GET /api/execution/alarms` | Current four-state alarms. Raw event severity does not imply a current alarm. |
| `POST /api/execution/alarms/{condition_id}/acknowledge` | Acknowledge one exact activation/revision. This records awareness only; it does not clear the condition or recover motion. |
| `POST /api/execution/alarms/acknowledge-all` | Acknowledge the exact returned membership/generation; stale membership is rejected. |

The four alarm states are `active_unacknowledged`, `active_acknowledged`,
`returned_unacknowledged`, and `returned_acknowledged`. An acknowledged active
alarm is still a standing abnormal condition. Only its catalogued source can
report return-to-normal. Recovery acknowledgement (`acknowledge_and_continue`)
is a separate, non-moving action in the current cycle recovery offer.

### Scene + visualization

| Endpoint | Use it when |
| --- | --- |
| `GET /api/scene` | The backend-authoritative scene. Use when the viewer looks wrong. |
| `GET /api/scene/viewer/status` | Viewer stream demand, client and renderer state. |
| `GET /api/scene/snapshot/render?width=&height=&format=` | Render the current 3D view as `jpeg` or `png`. Needs an open Composer viewer. |
| `GET /api/scene/snapshot/status` | Snapshot-capture counters and last rejection/timeout. |

The viewer settings (gear at the top right) set the robot update rate shown
in the browser. This changes only the display, never the robot's motion or a
pose source's acquisition rate.

### Diagnostic capture

| Endpoint | Use it when |
| --- | --- |
| `GET /api/debug/export?run_id=…&application_start_id=…` | **Single-call debug bundle** for a Program run; capture this for bug reports. |
| `GET /api/debug/export/ros2-medkit?run_id=…` | Normalized trace of a run in a ManyForge-native format (`manyforge.ros2_medkit_trace.v2`); no ros2_medkit tool reads it. |

## Status commands

- **`./scripts/compose-dev.sh status`** — the owner-issued current Scenario
  with `ready` and `unavailable_reasons`.
- **`docker inspect --format '{{.State.Status}} {{.State.ExitCode}}' manyforge-development-workbench`**
  — container state and exit code.
- **`./scripts/compose-dev.sh stop`** — stops the workbench and keeps a
  bounded console tail under the runtime root's `evidence/`.

## Environment variables that affect observability

| Env var | What it controls |
| --- | --- |
| `MANYFORGE_DEV_CONTAINER_NAME` | Workbench container name (default `manyforge-development-workbench`). |
| `MANYFORGE_MOTION_TRACE_DIR` | Motion-trace snapshots for debug export, in a capped store; empty keeps traces inline. Default: `/tmp/manyforge-state/motion-traces` in the workbench (see [`DEVELOPMENT_WORKBENCH.md`](DEVELOPMENT_WORKBENCH.md#reference)), the run's `current/motion-traces/` with the operator launcher. |

## Container names

The workbench runs as one container, `manyforge-development-workbench`
(override with `MANYFORGE_DEV_CONTAINER_NAME`), visible via `docker ps`. It
uses host networking to share DDS discovery with Isaac or the robot driver.

```bash
docker logs -f --tail 100 manyforge-development-workbench
```

## Capturing a bug report

For a reproducible failure, capture the following before stopping
(`API=http://127.0.0.1:9020`):

1. `GET $API/api/debug/export?run_id=<program_run_id>&application_start_id=<A>`
   — the single-call debug bundle. `<A>` is the 16-character
   `application_start_id` of the start the run belongs to.
1. `GET $API/api/execution/alarms` and `GET $API/api/cycle/status` so the
   alarm lifecycle and exact recovery offer are preserved.
1. `docker logs --timestamps --tail 2000 manyforge-development-workbench > workbench.log`
   and the `./scripts/compose-dev.sh status` output.
1. Note the start command (`./scripts/compose-dev.sh start --project …`).

Then, running or stopped, one bundle of the run's files:

```bash
./scripts/compose-dev.sh collect [--start <A> | --last] [--out bundle.tar.gz] [--max-bytes <bytes>]
# ./manyforge run: ./manyforge collect …
./scripts/compose-dev.sh collect --list   # one line per run, with its start id
```

It runs on the host, only reads the runtime state root and writes one
`.tar.gz` (default `manyforge-collect-<UTC>.tar.gz` in the current
folder) of one run: the newest (`--last`, the default) or the one of start
`<A>`. In this order, while the files' total stays within `--max-bytes`
(default 512 MiB, before compression): the run's `session.json` and
previous-run report, the console captures of its container under
`evidence/`, its History rows (`history.ndjson`), copies of its SQLite
journals, its assistant audit files, the rest of its run folder and the
motion traces written during it. A file that does not fit is left out;
`manifest.json` in the bundle lists what is in it and what was left out. The
History rows and the copies of a running run's journals need
`manyforge_headless_api/history_export.py` beside the launcher, which a
checkout has.

All History rows of one start or one Program run, oldest first, one JSON
object per line, from the running application:

```bash
docker exec manyforge-development-workbench python3 -m manyforge_headless_api history export \
  --application-start-id <A> [--program-run-id <n>] [--since 2026-11-24T14:00:00Z] [--until …] \
  --out /tmp/history.ndjson
docker cp manyforge-development-workbench:/tmp/history.ndjson .
```

Without `--application-start-id`, `--program-run-id` means a run of the
current start. `--history-file <state root>/history/history.v<N>.sqlite3`
reads a History file instead, without the application (from a checkout on
the host: `python3 -I manyforge_headless_api/history_export.py export
--history-file … --application-start-id <A> --out history.ndjson`); it never
writes beside the file. `--max-rows` (default 1,000,000) bounds the export;
when it is reached, the newest rows are kept and the command says so.
