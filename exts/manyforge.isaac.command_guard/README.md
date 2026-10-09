# Isaac stamped-command receiving guard

This extension is disabled by default. Its imports start no ROS executor,
monitor, graph or simulator effects. It replaces the previous custom JSON
command-frame foundation and its goal/binding/retirement protocol.

The command wire is the standard `sensor_msgs/JointState`. The only control
wire is `builtin_interfaces/Time` carrying the trusted Reset/startup hold
cutoff. There are no goal keys, custom messages, session tokens or frame ACKs.
The configured sole writer's joint names determine the command shape, with no
fixed six-joint or sixteen-joint limit. The immutable projection retains full
integer timestamps and position/optional velocity/effort arrays.

`CommandGuard(GuardConfig(joint_names, max_command_age_ns), local_clock)` starts
closed. The command age must be explicitly configured; there is no default
policy value. The bounded injected local monotonic reader runs inside short
state locks and must not perform SDK, ROS, I/O or waiting. Source stamps serve
only ordering: receipt age never subtracts clocks from different machines.
Every accepted source stamp must exceed the last accepted one and the latest
authorization cutoff. Equal/reordered stamps and earlier callbacks completing
later never refresh age. Expiry is checked before offering a queued frame and
before the writer consumes a pending command, including holds and factor0.
Clock regression and invalid/future local receipts close the guard.

`authorize(T)` is a trusted owner input, not evidence that Stop or hold happened.
The existing upstream owner first cuts over the old control, observes the
cancel's commanded hold, and supplies T from that controller's stamp domain.
Startup requires authorization after JTC becomes active and holding, before
homing. A strictly newer authorization discards pending commands and reopens
an expired guard; replayed/older authorization does neither. Frames never
reopen it. A restarted guard starts closed and requires fresh authorization.
A producer clock reset that cannot provide a newer cutoff remains closed;
there is no inferred epoch or automatic workaround.

`offer` copies no SDK targets. `take_latest` is called by the sole simulator
writer immediately before applying its immutable snapshot. It must not queue
the result to another writer. Authorization must be processed on that same
writer thread before consumption, so it cannot race an earlier reserved SDK
write. No guard state lock may cover an SDK call. `monitor` can latch expiry
independently of simulator progress; it cannot apply containment through a
stalled simulator thread. Local receipt age cannot detect arbitrary DDS transit
of increasing delayed source stamps. A Python monitor shares the GIL and
claims no independent deadline through Kit/GIL/process failure.

The existing scenes contain independent stock command subscribers and
ArticulationController physics writers. This foundation is **not wired to
those scenes**. It must replace that final receiving/writing path, arrange
persistent target containment before a resumed physics step, and preserve
independent joint-state publication before activation. Adding a second
subscriber next to the stock writer would not provide containment. Normal SDK
return is not proof of a target write, receipt, realization or physical rest.

Focused fake-clock tests run through the existing `scripts/test-all.sh python`
selector, which already discovers this extension. They cover startup, holds,
source order, expiry, timestamped Reset, restarted closure, immutable complete
frames and callback completion races. Python syntax supports Isaac's embedded
Python3.11. Pure tests do not qualify live pause, the sole writer, persistent
target handling, transport bounds or physical delivery.
