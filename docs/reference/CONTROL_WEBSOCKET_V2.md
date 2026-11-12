# Execution Control WebSocket v2

`/ws/control` is ManyForge's low-latency command and authoritative-status
session for the first-party UI and compatible external clients. Composer
proxies this session to Headless; it does not own Program, operation, Stop, or
Reset semantics. Program Runner and the execution host remain the
authorities.

The public browser endpoint is Composer. Headless exposes the same protocol on
its internal service boundary. External integrations may use the generated
REST API or this WebSocket when they need one persistent ordered status/control
session.

## Fixed profile

| Property | Value |
|---|---:|
| Protocol/schema version | `manyforge.execution_control.v2` |
| Heartbeat challenge period | 250 ms |
| Delayed classification | 500 ms without a confirmed heartbeat |
| Stale command cutoff | 1,000 ms without a confirmed heartbeat |
| Dead-peer reclamation | 30,000 ms without a heartbeat echo |
| Command freshness lifetime | 750 ms, measured by the receiving backend clock |
| Maximum connected control clients | 16 |
| Maximum client-message size | 65,536 bytes |
| Maximum client-message rate | 64 messages/s per session |
| Maximum complete snapshot size | 16 MiB |
| Maximum node-progress publication rate | 20 Hz, newest value only |
| Maximum manual-operation progress publication rate | 10 Hz, newest value only |
| History summary page | at most 32 rows; exact payload is detail-only |

Integer transport sequences and monotonic timestamps are positive decimal
`uint64` strings so JavaScript does not lose precision. These limits are one
measured product profile, not per-session tuning knobs.

The shell fact `held_run_draft_edit_allowed` (boolean) sits beside
`authoring_admission`: true only while a Program run exists, nothing of it is
moving and nothing can start. Headless derives it from the
same capture as `program_cycle` and `execution_control_state`; Composer
applies the same rule to each Draft edit, and the browser only reads it.

The optional shell fact `operation_progress` is a complete latest-only list
of executing single-participant manual operations (at most 4096 rows). Each
row has exactly `owner_epoch`, `operation_id`, `operation_revision` and
`progress` (finite, 0–1). The execution host's event stream supplies this list
from resident in-memory operations (re-read every 100 ms while one executes),
separately from its durable event rows: progress-only samples do not append
journal records. Headless coalesces changes
on its existing event-pump wake; no extra polling connection or cold-state
rebuild is involved. An empty list clears prior samples. The browser applies
samples only to an executing operation with matching owner, ID and revision;
lifecycle, Stop and terminal state always take precedence. Unchanged
progress produces no control publication.

## Cell facts

Besides `runtime`, `alarms`, `component_overview`, `authoring_admission`,
`held_run_draft_edit_allowed`, `execution_targets`, `execution_control_state`
and `program_cycle`, every owner `snapshot` carries the component model's
facts as shell keys. Each value is complete (every component, every signal),
and a `delta` carries a key again whenever its value changes; age fields
(`age_ns`, `observed_at_ns`) alone don't cause a delta. While the execution
host's component model can't be read, each of the four keys below is `null`
(unavailable), never its last value.

| Shell key | Value | Same value over REST |
|---|---|---|
| `component_state` | list of every `ComponentState` (`component_id`, `owner_id`, `role`, `lifecycle`, `fault`, `reason`, `activity`, `streams`, `capabilities`, `action`, `last_action`, `details`, `endpoints` (only while enabled and serving), `observed_at_ns`), in cell order | `GET /api/components` |
| `cell_latch` | `{latched, causes[]: {component_id, kind, fault?}}` | `GET /api/cell/latch` |
| `io_signals` | list of every `SignalSample` (`signal_id`, `value`, `source`, `age_ns`, `fresh`, `controller_owned`) | `GET /api/io` |
| `run_readiness` | `{can_execute, can_resume, motion_clear, blocked_reasons[]}` | `GET /api/program/readiness` |
| `global_rules` | list of `{rule_id, state, keys, unavailable_reason?}` per global interrupt rule; `null` until the rule catalogue is known | — |

`global_rules` comes from the execution host's event stream, like
`operation_progress`, and changes publish on the same wake.

## Two independent states

Version 2 deliberately separates transport liveness from backend semantic
availability:

- A small `heartbeat_challenge` / `heartbeat_echo` exchange proves that the
  browser, proxy, and Headless session can still communicate. It begins
  immediately after `server_hello` and continues while backend state is
  synchronizing or unavailable.
- Each challenge carries `semantic_status` equal to `current`,
  `synchronizing`, or `unavailable`. A non-current status has one bounded
  `semantic_reason` and no command freshness. A current status has one opaque
  backend `freshness` context and no reason.
- Heartbeat never reads an owner, builds Component Overview, reduces or
  serializes full state, appends History, performs Viewer work, or updates a
  browser React store. It reads only cached transport/semantic metadata.

Semantic unavailability therefore fail-closes affected commands without
claiming that the connection failed. Conversely, a delayed or stale heartbeat
is a genuine transport/browser-processing diagnosis and is not hidden by
changing semantic state.

## Session flow

1. The client sends one `client_hello` naming exactly v2, a stable browser
   incarnation, and a bounded source label. The browser identity survives a
   socket reconnect so one incident can be resolved without creating a second
   activation; it grants no authority.
2. The server replies with `server_hello`, subscribes that client to the shared
   demand-scoped projection, and immediately starts heartbeat. A complete
   `snapshot` follows as soon as the backend projection is current. Heartbeat
   remains live if that takes longer than the stale threshold.
3. The server retains at most one unanswered challenge. The client echoes it
   with its next contiguous client sequence. A paused client cannot accumulate
   heartbeat history or receive a catch-up burst.
4. A `command` carries the same session/client sequence in its outer message
   and a closed `manyforge.execution_command.v1` envelope. The envelope names
   exact scope, source attribution, expected semantic owner/revision, and the
   opaque freshness context from a current heartbeat.
5. Every challenge includes `received_client_sequence`, the greatest client
   message received by Headless. This watermark lets the browser detect an
   unconfirmed command delivery while heartbeats otherwise continue. The
   browser never retries that command automatically.
6. The one-second command-delivery deadline applies to the server-received
   watermark. A covering watermark proves that Headless received the command,
   ends that transport deadline, and never proves execution effect. A missing
   watermark rejects the pending UI action as unconfirmed delivery, disables
   ordinary controls, and forces one current-snapshot resynchronization; the
   browser never retries the command. The mandatory correlated
   `command_receipt` may follow later with the owner's disposition, while
   authoritative lifecycle state continues to stream independently. The
   browser imposes no lifecycle timeout, holds no backend state behind the
   receipt, and rejects an unresolved receipt if the session disconnects.
7. Server messages after hello have one increasing sequence in actual wire
   order. Owner replacement produces `resync_required`; the client replaces
   that session by reconnecting, receiving current state only. No in-band
   `resync` command or replay of motion/commands exists.
8. Every owner `snapshot` or `delta` carries a separate `state_sequence`. Once
   the browser has validated and merged that value, it returns
   `state_applied` for the exact sequence. Headless sends no later owner state
   to that browser until this credit arrives; newer pending state replaces the
   older pending value. Receipts and heartbeat remain independent and can pass
   an uncredited state. Credit is per browser session, so a slow browser cannot
   block another browser.

Client message kinds are `client_hello`, `heartbeat_echo`, `state_applied`,
`command`, and `client_incident`. `client_incident` accepts only the small
published enum of browser-only transport/render/history/viewer failures;
Headless supplies time and attribution and cannot be used as a general event
writer. The v2 client enum is `browser_render_delayed`,
`command_delivery_unconfirmed`, `control_disconnected`,
`control_projection_invalid`, `history_request_failed`,
`history_detail_request_failed`,
`viewer_model_unavailable`, `viewer_scene_unverified`,
`viewer_observation_stale`, and `viewer_geometry_unavailable`. The viewer kinds
record an unavailable or unverifiable view of the applied robot model, Scene,
live poses or required geometry while the browser presents it as an error;
they are reported once on activation and once on resolution, and the expected
gap of a known controlled Load/Apply transition is not reported. Headless itself
records `control_delayed` and `control_stale` from measured challenge RTT.
Each threshold has one bounded incident episode across consecutive delayed
challenges. A late echo proves liveness but does not resolve delay; one round
trip below that threshold resolves it, recording the original incident ID,
first/last observation, affected challenge count and maximum delay. Closing
the session records its termination, not a fabricated healthy round trip.
Recoverable Runner-frame or filesystem-source failures are separate
`control_source_unavailable` incidents recorded by Headless, never by clients.
They keep independent heartbeat alive and recover from one coherent current
capture in the existing source loop; no browser churn or command retry is
required. Recorder failure is observable in bounded diagnostics and logs.
Server message kinds are `server_hello`, `heartbeat_challenge`, `snapshot`,
`delta`, `command_receipt`, and `resync_required`. Exact closed DTO validation
is owned by `manyforge_headless_api/control_protocol.py`. Unknown fields,
another session/version, sequence gaps, stale freshness, size excess, and rate
excess fail closed before ordinary owner state changes.

For a `delta`, `control` and owner `state_sequence` are present together.
Composer-local Draft deltas carry neither an owner sequence nor a received
watermark and have `shell.complete=false`. Explicit null sequence/watermark
fields are not substitutes for omission. Owner deltas also have an incomplete
shell and retain each omitted validated immutable domain.

A receipt's `received_client_sequence` is the cumulative watermark at emission,
so it may exceed its exact `receipt.client_sequence` when a heartbeat or scale
message overtakes owner completion. It cannot regress or fall below that exact
command. Only the matching pending command/session/correlation is resolved by
the receipt; a higher watermark is not an unrelated command's outcome.

## State construction and publication

While at least one client is connected, Headless retains one immutable newest
fact from each authoritative domain. One reduction uses each captured fact
exactly once for both Component Overview and the rest of the shell. It never
rereads Program through Component Overview, waits for unrelated generation
counters to match, or retries a whole aggregate in search of coincident
samples.

Program-cycle changes publish immediately. Node progress is replaceable and
bounded to 20 Hz. Cold/component presentation publishes only when its
operator-visible value or availability class changes; source timestamps,
heartbeat counters, receiver sequences, and derived age do not create frames.
The first owner state is one complete snapshot. Later owner state is a sparse
top-level fact delta marked `complete: false`; the browser replaces only the
facts present in that delta. Execution targets are immutable within their exact
execution owner/product identity, so control-only changes reuse the last
validated target instead of retransmitting it. A target change still carries
the control state from the same execution-host capture, and the browser rejects any
identity mismatch. The per-session `state_applied` credit keeps at most one
delivered but unconsumed owner state and one newest pending replacement end to
end, including
inside the browser's ordered WebSocket delivery queue.
The Viewer uses its separate applied-frame-credit WebSocket. No control client
means no control-source subscription, reduction, serialization, heartbeat, or
diagnostic timer.

At the public Composer endpoint, a committed edit to the shared Program,
Deployment, or Scenario authoring workspace adds one tiny latest-only `delta`
containing `authoring_workspace`: the composite workspace epoch/revision and
both Draft identities. It does not resend the runtime shell or wait for a
runtime update. Its shell is marked `complete: false`, so it cannot be consumed
as a complete runtime snapshot.
Composer assigns the public downstream `server_sequence` across both proxied
Headless messages and this local delta. The opaque Headless freshness context
is forwarded unchanged and remains the only context accepted by Headless for
commands.

## Commands and lifecycle

The socket is a transport adapter, not a command queue or lifecycle state
machine. Browser WebSocket, REST, and local/optional ROS 2 or field adapters
translate the same generated command DTO into one `ExecutionControlIngress`.
Program Runner or the Kernel atomically orders concurrent requests against
current state and exact resource scope.

- Ordinary commands are never queued until legal and never replayed after a
  timeout or reconnect.
- Each browser session has at most one pending discrete ordinary command and
  one coalesced runtime-scale update. Stop/Full Stop keeps reserved capacity.
- Repeated Run cannot create a second current cycle. Repeated Stop joins or is
  a no-effect success after proven rest. Repeated Reset joins the exact reset
  already in progress.
- Reset accepted during motion publishes backend-owned `resetting`, invokes
  the execution host's controlled stop, waits for certified settlement, restores the
  authoritative initial scene, and publishes Idle only after completion.
- The browser mirrors backend lifecycle and `allowed_commands`; it never
  invents Idle, Starting, Running, Stopping, Resetting, terminal, recovery, or
  synchronization completion.

Exact Stop remains attemptable through its reserved path whenever the
transport and target permit it. A browser is not a safety-rated stop path; a
stale or disconnected browser must direct the operator to commissioned plant
safety.

## Backpressure, reconnect, and diagnostics

Delivery priority among already-ready items is receipt, heartbeat, then newest
replaceable state. Owner state is never withheld merely because command work or
its correlated receipt is still pending. An
exact browser `state_applied` credit permits the next owner state; it is
transport backpressure, not semantic acknowledgement. One slow client has its
own bounded slots and credit and cannot delay another client or owner
publication. Disconnect does not stop an autonomous Program. Reconnect creates
a new session and receives current state only.

`command_receipt.result` is optional command-specific data, not a copy of the
current owner projection. Current in-tree consumers require it only for the
compact runtime-scale acceptance value. Program, operation, Stop, Reset, and
recovery receipts omit their otherwise redundant result snapshots; consumers
read their outcome from the owner-published state. Receipt identity/revision
names the exact owner context used for the decision. Headless does not reread
the owner after dispatch to decorate a receipt with potentially unrelated newer
state.

Headless measures challenge-to-echo latency on its own monotonic clock. It
records sparse incident transitions, never individual heartbeats: one
activation and one resolution summary per delayed/stale episode, with bounded
count and maximum delay. Browser-only incident kinds use a strict bounded
reporting contract; the server supplies authoritative attribution and time.
Its fixed-size correlation cache may forget the oldest abandoned browser
episode after the activation is durable; a later unmatched resolution is a
diagnostic no-op and can never close or degrade control. There is no lifetime
incident counter.
Runtime History lists compact summary rows and loads an exact event payload
only when a sequenced row is selected. The C++ Historian builds that summary
outside the Python control loop, and Composer relays its bounded JSON without a
second parse/encode cycle.

## Security and attribution

Composer enforces same-origin WebSocket use unless an origin is explicitly
configured. Production authentication/TLS and trusted-principal stamping
belong at deployment ingress. A caller source label is attribution, not
authority. Local button, PLC, fieldbus, or ROS 2 adapters own their transport,
authentication, and debounce and then call the common ROS-free ingress.
Hardware emergency stop remains independent.
