# Isaac command guard foundation

This package contains pure command-frame validation and receiving decisions.
Imports start no ROS executor, monitor, graph or simulator effect. The extension
is disabled by default and has no startup component. Production motion and
retirement receipts remain unavailable until the final receiving path is wired
and qualified. Its Python syntax and standard-library dependencies support 3.11.

The one wire is a fixed-schema JSON object in `std_msgs/String.data`, schema
`manyforge.isaac.command_frame.v1`. `frame.py` defines its complete fields and
canonical encoder. Full Core goal/binding identities, configured owner epoch
and receiver occurrence travel with ordered joint names/positions, producer
sequence, consumed goal/scale/Stop generations and the final marker. Integer
counters remain uint64 integers, including values above 2^53. All schema fields
are mandatory; unknown/duplicate fields, unsafe types and nonfinite values are
refused. Encoding/decoding happens outside the controller's realtime loop.

`GuardConfig` requires exact joints, identities, byte/string bounds and command
age. `CommandGuard(config, local_monotonic_clock)` also requires one trusted
bounded clock reader (for example `time.monotonic_ns`). It samples that reader
inside its short locked check, so independently serviced callers cannot reverse
clock observation merely by acquiring the lock in a different order. Receipt
time remains the separate callback-entry sample; the clock reader must perform
no SDK, ROS, I/O or waiting. It invents no profile timing value. Shared maxima are 16 joints and four
outstanding keys. The String DDS type itself is unbounded: the decoder cap
bounds accepted application work/storage, not RMW deserialization resources.
Transport caps, executor scheduling and source transit assumptions still need
qualification. A fresh local receipt cannot prove fresh generation after an
arbitrarily delayed DDS delivery. Source sequence must come from actual generated
controller frames, never a publication counter or repeated cached positions.

The trusted upstream occurrence/sequence ingress admits keys with `admit` and
explicitly selects an owned `EXECUTING`, `BRAKING` or `HOLDING` operation with
`arm`. Incoming frames never admit/arm keys or clear containment. Repeated arm
calls for the same key do not renew age. Intentional startup/idle has no armed
operation and requires no stream. Changing the selected key is an explicit
upstream owner action, not a transition inferred by this device guard.
`offer` receives one completely decoded immutable frame with its callback-entry
local monotonic timestamp. Increasing actual producer sequence renews age;
repeated/reordered frames do not. `monitor` checks age on that same local clock,
including zero-scale holding. Expiry is checked before accepting a queued fresh
sequence and before reserving the first effect after any long gap. Clock
regression or invalid receipt type/negative/future time closes the occurrence.
An older callback-entry receipt or reordered producer sequence is stale evidence:
it is refused without renewing age or manufacturing a clock fault. Trips are sticky;
there is no recovery/reopen method. The owner must establish a new occurrence
normally after closure. Fake-clock gaps test this logic, not live pause ordering.

All state locks are short and contain no SDK/ROS/I/O callback. `begin_effect`
reserves the latest immutable frame in the single shared in-flight slot;
the owned simulator thread performs the immediate effect outside the lock;
`finish_effect` consumes only that exact token and named `RETURNED`, `FAILED`
or `UNKNOWN` result. Tokens must not be queued to a different writer or replayed.
The caller must always finish an attempt, including an exception/unknown outcome.
A stalled effect leaves the slot occupied while monitor/fence requests remain
serviceable. Later frames can replace the pending snapshot but cannot bypass
in-flight serialization. RETURNED means the call returned: Isaac SDK methods
may return without writing on an invalid handle, so it is not application,
delivery, realization, final completion or physical-rest evidence.

`request_fence` immediately discards pending frames and retains the exact key
and fence sequence, including when it overtakes admission. A different request
sequence conflicts with the retained request. `begin_fence_effect` reserves
configured persistent-target handling only after all earlier in-flight work
returns; `finish_fence_effect` records its exact typed boundary fact.
A normal target write or absence of graph execution is not a qualified fence.
`FenceBoundaryOutcome.RETURNED` records only SDK return. The sole local
receiving adapter may supply `FENCED`/`DRAINED` only after its qualified contract
establishes rejection of queued/later commands and handles persistent targets.
The guard retains that correlated fact for the existing mechanism retirement
seam. `read_fence_fact(key)` exposes it until owner release even if other keys
subsequently return; observation never repeats effects. This unwired foundation has no production boundary adapter or capability. This package emits no per-frame physics ACK.

Only after that actual boundary completion and upstream retirement observation
may the trusted owner call `release_after_retirement` with the exact returned
fence token after observing the guard's own retained `FENCED`/`DRAINED` fact.
Caller-supplied status, SDK RETURNED or a requested fence cannot authorize release. This removes both storage and admission; queued frames for the old
key remain unknown/unarmed. The existing upstream occurrence/sequence ingress
must never reauthorize a retired identical key. Four outstanding records do not
form a lifetime replay journal. A fence cannot be forgotten merely to admit a
submission; capacity exhaustion closes acceptance. Release never clears a trip.

Integration must serialize the sole final application path, replacing the
articulation controller's independent physics tick and stock command subscriber
wiring. A separately serviced local monotonic monitor may latch during a paused
or stalled simulator, but cannot write targets through a stalled effect thread.
A Python thread shares the GIL; this foundation claims no independent bounded
containment under a Python/Kit stall, process death or scheduler starvation.
The configured hold, persistent targets, first resumed step, graph reset, source
transit bound, stock String ABI and performance remain integration gates. No
scene, bridge, historical Boolean gate or launch file is changed by this slice.

## Identity projection

All `ExecutionGoalKey` fields from
[`execution_goal_key.hpp`](../../manyforge_core/include/manyforge_core/execution/execution_goal_key.hpp)
are preserved: operation id, controller goal id/generation, prepared execution
id, and binding configuration generation/provider epoch. These are uint64
counters and exact strings, not parsed operation ids, hashes or narrowed handles.
The additional exact address comes from the verified selected participant's
[`RuntimeParticipantBindingDTO`](../../manyforge_core/include/manyforge_core/runtime_participant_binding.hpp):

| Frame address field | Existing selected participant field |
| --- | --- |
| `control_binding_id` | `control_path.control_binding_id`: retained controller binding |
| `control_route_id` | `control_path.control_route_id`: selected control route |
| `active_control_configuration_id` | `control_path.active_control_configuration_id`: active configuration identity |
| `provider_instance_id` | `control_path.provider_instance_id`: execution provider instance |
| `provider_runtime_unit_id` | `control_path.provider_runtime_unit_id`: provider's runtime unit |
| `command_target_id` | `command_target_id`: selected command target |

The config compares that entire address exactly in addition to the full key's
configuration generation and provider epoch. These values fence distinct routes,
providers and targets that otherwise share joints or counter values. The
upstream retained active configuration owns route/revision validity; the guard
does not re-resolve bindings or copy model/collision/planner state. Configured
joint order is the exact selected command-resource projection, not expanded
model membership. CCLC wiring must additionally bind the sole graph/prim
application path to this selected address before enabling effects.

## Focused tests

The existing `scripts/test-all.sh python` pytest list includes this package's
stdlib unittest cases. A narrow Docker check can run
`python3 -m pytest exts/manyforge.isaac.command_guard/tests/test_command_guard.py`.
No Isaac or ROS module is imported by these tests. Fake qualified boundary
outcomes test correlation and release ordering; they do not qualify any real
simulator boundary, monitor scheduling or physical rest.
