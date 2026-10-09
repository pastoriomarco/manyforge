# ManyForge Isaac hardware

This package supplies the `manyforge_isaac_hardware/IsaacSystem` ros2_control
plugin. The current slice supports observation while inactive. Activation
returns `FAILURE`, retaining the inactive lifecycle state: the canonical
binding, lease and accepted-command path is not installed. There is no command
publisher. This package is not connected to a launch entrypoint or a running
simulator.

The plugin requires exactly six distinct joints with one position command
interface and a position state interface each. Velocity state may be declared
for all six joints. Mimic joints, other state/command types, sensors, GPIOs and
asynchronous hardware updates are rejected. Authored initial values are never
used as observed state or command targets. Exported state begins as NaN and
returns to NaN whenever observation is unavailable or ineligible.

Required hardware parameters are `joint_states_topic`, `feedback_source_clock`,
`source_max_age_ns` and `receive_max_age_ns`. The source clock must explicitly
select `system` or `ros_simulation`. Both age limits are positive decimal integer nanoseconds;
their values must come from the selected binding's qualified profile. No
universal timing limits are supplied here. The raw JointState must contain
exactly the six configured names and finite, complete position/required
velocity arrays. Source header stamps must advance and satisfy the source age
limit at receipt, in their selected clock domain. Names may arrive in any order.
Continuing eligibility expires by local monotonic receipt age. Simulation-clock
age is never added to steady-clock elapsed time: simulation rates may differ.
Missing, malformed, repeated, older or stale source stamps never renew evidence.
A source or read clock reversal latches ineligibility until reconfiguration.
System-clock stamps must be nonzero. A zero simulation stamp is allowed only
after the observer has received an explicit `/clock` message, including zero.
A bridge that replaces physics timestamps cannot establish fresh physics
evidence through this check.

Simulation source mode uses one explicit `/clock` subscription on the same
NRT executor as feedback. It tracks initialization and invalidates observation
on a backward clock/reset even without a subsequent joint sample. The node
clock is fixed to system time; this source selection changes neither controller
time nor execution progress. `/clock` and joint feedback have no guaranteed
cross-topic order: a sample ahead of the last observed clock is refused, and
can become eligible only through a subsequent received sample that validates.
Actual clock/sample ordering, queue lag and reset behavior remain unqualified.

One SingleThreadedExecutor handles ROS subscription work outside read/write.
The callback is the only producer of an eight-sample preallocated SPSC queue;
read is its only consumer. Release/acquire ownership prevents producer edits
to a slot being consumed. A full queue drops the new sample. Read takes the
latest of at most eight pending samples; overflow invalidates observation and
drains that bounded backlog, requiring a subsequent source sample. No queue is
reset until its producer has stopped. read/write perform no ROS calls,
allocation, logging or blocking; read uses a local steady clock and fixed
array operations. Lifecycle cleanup cancels and joins the observation worker
outside that path. The worker's finite wait closes the cancel-before-spin race.
Configure/worker exceptions report their first cause with component identity
outside the tick; failure stops observation without an automatic restart.

Every write is suppressed while inactive. Its OK return permits maintaining an
inactive hardware interface and proves no command submission or realization.
The local publication fence prevents future emission; it proves neither
delivery nor draining of DDS or Isaac queues. Internal transport facts are
implementation seams, not a public execution status or wire API. No motion
law, execution outcome, Stop selection or recovery policy lives here.

Focused deterministic tests exercise the actual JointState callback conversion,
validation and handoff, source/local ages, rejected raw arrays, queue overflow,
concurrent slot ownership, initial-state refusal and activation refusal. They
use injected times and no sleeps. Build and run tests in the selected Jazzy
image with hardware_interface 4.48.1. These tests do not qualify RT timing,
downstream command consumption, playback/reset behavior, live Isaac or hardware.
