# Isaac stamped-command receiving guard

This extension is disabled by default. The pure package imports start no ROS
executor, monitor, graph or simulator effects. Enabled Kit startup owns its ROS
context and a scoped position writer. It replaces the previous custom JSON
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

The receiving adapter projects the complete configured arm from a shared
JointState. Supersets are ordered by configured names; disjoint gripper frames
are ignored without age refresh. Partial/duplicate/nonfinite arm frames are
refused. Only positions and explicit named DOF indices reach ArticulationAction;
optional velocity/effort arrays are never new SDK effects.

Enabled startup requires four explicit per-scene settings: `articulation_prim_path`,
`joint_names` in JTC order, `command_topic`, and `max_command_age_ns`.
The Time and status topics derive as `<command_topic>/authorization` and
`<command_topic>/guard_status`. There is no default age. The host lane profile
retains the same command topic and Manager passes it to the ROS launch; it does
not author the simulator's prim, joint list or age. ROS bridge dependency startup
selects Jazzy before this extension uses the installed bindings. Its context,
node and executor are owned separately from the bridge context. The Time
subscription is created only after the selected prim, SDK physics handle and
exact name-to-DOF map are ready. The sole pre-physics callback handles pending
authorization before the final frame take and immediate scoped SDK call.
No SDK initialization, queue or ROS publication follows that final take.

DiagnosticStatus publishes closed/open/tripped changes with exact integer
`authorized_after.sec` and `authorized_after.nanosec` when an authorization
exists; absence is not fabricated zero. New authorization while open is a new
status. Every status also includes `joint_names`, a compact JSON ordered string
array matching the configured JTC order, with standard JSON escaping. The host
parses this list and compares it exactly with `ReadLaneFacts().joint_names`;
missing, invalid, differently ordered or different names block authorization
confirmation and motion with a diagnostic. Reporting serializes immutable prior/result facts outside guard locks;
the monitor can latch a trip while publication or SDK blocks. Publication errors
have unknown delivery, retain failed facts and close local admission without
automatic retry. Host waits for exact-T open before any motion and retains an
observed trip until Reset. Status is neither SDK effect nor physical-rest proof.

Stage close discards frame/Time and quiesces owned callbacks before destroying
their subscription. The same guard retains authorization/order floors and trip.
The age monitor derives its interval from the required age, shares Python's
GIL, and makes no process-stall/physics-pause deadline guarantee. One already
reserved SDK call may finish after a concurrent trip or close.

The isolated scene worktree contains reviewed arm-writer disconnections;
canonical scene cutover and live qualification remain pending.
**Keep this extension disabled until that integrated cutover.**
Disconnect only the addressed arm writer, preserving gripper and feedback.
Adding a second subscriber beside that writer would not provide containment.
On trip this writer stops applying; the last position target persists and the
arm may continue converging to it. No measured-target rewrite or rest claim.
Normal SDK return is not proof of a target write, receipt or realization.

Scene startup uses the scene repository's `config/command_guard/*.toml` through
Kit's native `--merge-config=<file>` mechanism, passed explicitly by the existing
Isaac operator. It keeps the existing app/process owner; no scene guessing or
custom configuration parser is introduced. The checked-in configurations remain
disabled and omit command age until the live route measurement and maintainer
selection. Kit supports merging TOML/JSON startup settings; actual merged Kit
startup remains part of live qualification ([NVIDIA CLI documentation](https://docs.omniverse.nvidia.com/kit/docs/carbonite/latest/docs/Kernel/CommandLineOptions.html)).

Isaac URDF initial values are not generated from the scene-start pose. JTC
activation holds at actual Isaac joints and this writer remains closed
until the host's hold authorization. Plugin seed publications before that
boundary are not containment proof. Offline mock still uses the deployment's
retained initial joints; this extension does not seed or rewrite any joints.

Focused fake-clock/fake ROS/SDK tests run through the existing `scripts/test-all.sh python`
selector, which already discovers this extension. They cover startup, holds,
source order, expiry, timestamped Reset, restarted closure, immutable complete
frames, callback completion races, exact status/error facts, context teardown
and scoped DOF effects. Python syntax supports Isaac's embedded Python3.11.
Fake tests do not qualify live Kit, pause, the authored sole-writer cutover,
persistent target handling, transport bounds, step-free reopening or delivery.
