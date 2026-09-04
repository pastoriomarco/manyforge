# Execution Control WebSocket v1

`/ws/control` is ManyForge's low-latency control and authoritative-status
session for the first-party UI and compatible external clients. Composer
proxies the session to Headless without owning Program, operation, Stop, or
Reset semantics. Program Runner and the native Kernel remain the semantic
owners.

The public browser endpoint is the Composer endpoint. Headless exposes the
same endpoint on its internal service boundary; deployments should not expose
that internal listener directly. An external integration may instead use the
REST workflow in [REST_API.md](REST_API.md), or implement this WebSocket
protocol when it needs a persistent ordered status/control session.

## Fixed product profile

| Property | Value |
|---|---:|
| Protocol/schema version | `manyforge.execution_control.v1` |
| Heartbeat challenge period | 250 ms |
| Delayed classification | 500 ms without a confirmed heartbeat |
| Stale command cutoff | 1,000 ms without a confirmed heartbeat |
| Dead-peer reclamation | 30,000 ms without a heartbeat echo |
| Command freshness lifetime | 750 ms, measured only by the receiving backend clock |
| Maximum connected control clients | 16 |
| Maximum client-message size | 65,536 bytes |
| Maximum client-message rate | 64 messages/s per session |
| Maximum complete snapshot size | 16 MiB |
| Maximum node-progress publication rate | 20 Hz, newest value only |

These are protocol constants, not runtime tuning knobs. Integer transport
sequences and monotonic timestamps are canonical positive decimal `uint64`
strings so JavaScript cannot lose precision.

## Session flow

1. The client sends one `client_hello` naming exactly version v1, a fresh
   client incarnation, and a bounded source label.
2. The server returns `server_hello` with its receiver/session incarnations,
   then keeps that one transport open while it obtains one coherent complete
   `snapshot`. Transport establishment and owner-state synchronization are
   distinct: if synchronization takes longer than 1,000 ms, the client shows
   a synchronization-delayed warning and keeps ordinary commands disabled; it
   does not reconnect merely because owner state is not current yet.
3. The server sends `heartbeat_challenge`; the client returns
   `heartbeat_echo` with the next contiguous client sequence. A session is
   ready for ordinary commands only after a complete snapshot and a confirmed
   heartbeat round trip. Operational heartbeat delay/staleness timing begins
   only after the snapshot barrier; a real transport disconnect is still
   detected while initial synchronization is pending. The server keeps at
   most one unanswered challenge. A delayed browser therefore cannot build a
   heartbeat backlog or receive a catch-up burst when it resumes. If owner
   projections temporarily cross during Reset or recovery, challenge issuance
   pauses and commands become stale-disabled; the coherent current snapshot
   and a fresh round trip recover the same socket. Only an issued unanswered
   challenge can classify the browser itself as dead.
4. A `command` carries the same session/client sequence in its outer message
   and its closed `manyforge.execution_command.v1` envelope. The envelope also
   carries correlation/source attribution, the expected semantic-owner epoch
   and control revision, and the current opaque backend freshness context.
5. `command_receipt` reports the exact receiver sequence/time and the owner's
   immediate disposition. Subsequent `snapshot` or `delta` messages carry the
   authoritative lifecycle and `allowed_commands`; receipt delivery alone
   never proves motion, settlement, or completion.
6. Server messages after hello have one strictly increasing sequence in
   actual wire order. A gap may be reconciled from current state; a duplicate
   or regression invalidates the session. Owner replacement produces
   `resync_required`; the client requests `resync` or reconnects and receives
   current state, never missed command or motion history.

Client message kinds are `client_hello`, `heartbeat_echo`, `command`, and
`resync`. Server message kinds are `server_hello`, `heartbeat_challenge`,
`snapshot`, `delta`, `command_receipt`, and `resync_required`. The closed DTOs
and their exact validation rules are owned by
`manyforge_headless_api/control_protocol.py`; unknown fields, unsupported
versions, invalid sessions/sequences, stale freshness, oversized messages,
and rate excess fail closed before ordinary owner state changes.

## Command and lifecycle semantics

The socket is a transport adapter, not a queue or state machine. All sources—
browser WebSocket, generic REST, named REST adapters, and local/optional ROS 2
adapters—enter the same `ExecutionControlIngress`, which has one bounded
ordinary lane and reserved Stop capacity. Concurrent sources are linearized
by Program Runner or the Kernel against current state and exact scope.

- An ordinary command is never held until it becomes legal and is never
  replayed automatically after timeout or reconnect.
- Each session admits at most one unconfirmed discrete ordinary command and
  one unconfirmed runtime-scale update. They progress independently, so Run
  never waits for a velocity update and a velocity update can immediately
  scale newly started motion. Newer slider samples remain coalesced in the
  browser; no command queue is introduced.
- Repeated Run cannot create another current cycle. Repeated Stop joins or is
  a no-effect success after proven rest. Repeated Reset joins the exact Reset
  already in progress.
- Reset accepted during Running publishes backend-owned `resetting`, closes
  Run/Resume/ordinary Stop admission, invokes the native controlled-Stop path,
  waits for certified settlement, restores the authoritative initial scene,
  and publishes Idle only after completion. The native operation may be
  Stopping while the Program remains Resetting; those scopes are intentionally
  distinct.
- Exact live Stop remains conservative even when ordinary presentation
  freshness is stale. A browser is not a safety-rated Stop path; unconfirmed
  Stop directs the operator to commissioned plant safety.

The frontend shows transport delay, suppresses a duplicate ordinary click,
and disables commands on a delayed/stale session. It does not reconnect merely
because the one-second command cutoff was crossed: the same socket recovers
only after a new echo and its subsequent server confirmation. A genuinely
unanswered peer is reclaimed after 30 seconds. The frontend must only render
lifecycle states and command eligibility supplied by the backend.

## Delivery, reconnect, and backpressure

Control receipt delivery and current control state have bounded outbound
slots; heartbeat has one outstanding challenge. Delivery priority is ordered
receipt, heartbeat, latest state: sustained replaceable node progress cannot
starve liveness, while command disposition remains first. The state slot
replaces an older unsent value with the newest one, and discrete receipts
remain ordered and bounded. Command receive/handoff does not await browser
writes, projection assembly, history, or motion completion. Viewer transforms
use the separately credited `/ws/scene/viewer` plane.

Disconnect does not stop an autonomous Program. Reconnect creates a new
session and sends current state only. No connected control client means no
Headless control-source subscription, heartbeat, shell assembly,
serialization, or delivery work. When the last client leaves, Headless fully
awaits cancellation of its Program-state, cold-state, and file-watch sources
before a replacement client may start a new source generation. At most one
upstream generation therefore exists, including during rapid disconnect and
reconnect sequences.

## Security and attribution

Composer enforces same-origin WebSocket use unless an origin is explicitly
configured. Production authentication/TLS and trusted-principal stamping
belong at the configured deployment ingress; a caller-provided source label is
attribution, never authority. Local button, PLC, fieldbus, or ROS 2 adapters
must apply their own transport/authentication and input-debounce rules, then
call the same ROS-free ingress. Hardware emergency stop remains independent.
