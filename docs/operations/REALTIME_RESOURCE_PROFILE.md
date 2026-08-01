# Real-Time Resource Profile

> **Cutover status:** retained as a restoration and behavior reference. The
> command recipes below are not qualified on the current `dev` branch and
> must not be executed until the selected N/I/R execution gate passes.
> Present-tense runtime and probe descriptions below record the captured
> policy, not a currently scheduled product lane.

The captured runtime provided an opt-in Linux resource profile for robot
deployments. It partitioned the ManyForge and ROS process threads, applied
explicit scheduler policy and CPU affinity to named runtime workers, and
failed startup when Linux rejected a required request.

This profile is deployment infrastructure, not a safety algorithm. It does not
move collision, stop, execution, or controller authority, and it does not put
Python on a real-time thread.

## Authority and priority order

The required order is:

1. `controller_manager` update/JTC command streaming;
2. ManyForge `ControllerCommand` callbacks;
3. ManyForge `RuntimeSafety` collision callbacks;
4. ManyForge `StopControl` deadline/follow-up callbacks; and
5. all planning, validation, Composer, Python, DDS, recorder, viewer, and other
   non-real-time work.

The JTC update loop remains the highest-priority robot command path. A
ManyForge collision result requests the existing stop path; it never preempts
or replaces JTC command streaming with Python work.

## Enable the strict application profile

Export the profile before launching or restarting the selected lane. This Thor
Isaac/ROS example uses CPUs 0-10 for non-real-time work, CPU 11 for runtime
collision, CPU 12 for ManyForge command/stop callbacks, and CPU 13 for
`controller_manager`:

```bash
export MANYFORGE_RT_RESOURCE_PROFILE=strict
export MANYFORGE_NRT_CPU_AFFINITY=0-10
export MANYFORGE_CONTROLLER_MANAGER_CPU_AFFINITY=13
export MANYFORGE_CONTROLLER_MANAGER_THREAD_PRIORITY=50
export MANYFORGE_CONTROLLER_MANAGER_LOCK_MEMORY=true
export MANYFORGE_RT_SCHED_POLICY=FIFO
export MANYFORGE_RT_CONTROLLER_COMMAND_PRIORITY=45
export MANYFORGE_RT_CONTROLLER_COMMAND_CPU_AFFINITY=12
export MANYFORGE_RT_RUNTIME_SAFETY_PRIORITY=40
export MANYFORGE_RT_RUNTIME_SAFETY_CPU_AFFINITY=11
export MANYFORGE_RT_STOP_CONTROL_PRIORITY=35
export MANYFORGE_RT_STOP_CONTROL_CPU_AFFINITY=12

MANYFORGE_FRONTEND_SOURCE_MODE=worktree ./scripts/launch.sh restart
```

Choose CPU sets from the actual target topology and load evidence. Do not copy
the Thor layout onto AGX Orin or another target without qualification.

The native-only lane has no `controller_manager`. Its launcher marks that
authority absent before validation, so the otherwise reserved controller CPU
may be returned to the NRT set (for the layout above, `0-10,13`). The
ControllerCommand > RuntimeSafety > StopControl order and their disjoint
reservations remain mandatory. Selecting the Isaac/ROS lane marks
`controller_manager` present again and rejects that overlap.

The launcher validates syntax, online CPU availability, pairwise isolation,
and priority order before a container starts. The strict Compose override
grants `SYS_NICE` and `RLIMIT_RTPRIO` to the two ROS services; the native-only
manual development container receives the same minimum scheduler grant and
runs its Composer main thread on the NRT set. Unlimited memlock is granted only
to the controller container. `controller_manager` applies its official
`lock_memory`, `thread_priority`, and `cpu_affinity` parameters. ManyForge's
three named C++ workers independently validate their owned priority/CPU
relation and apply their own policy and affinity before accepting callbacks. A
rejected required request aborts kernel construction.

The Composer process is deliberately not memory-locked. Locking a Python heap
with `mlockall(MCL_FUTURE)` can make unbounded memory resident or turn later
allocation into failure. Planning, validation, UI, DDS, and recorder threads
remain `SCHED_OTHER` on the non-real-time CPU set.

## Verify what Linux actually applied

Requested settings are not evidence. Run both probes against the live lane:

```bash
docker exec manyforge-composer-ros-1 \
  python /workspace/manyforge_core/tools/realtime_resource_profile_probe.py \
  --target composer --command-line-token 'python -m manyforge_composer'

docker exec manyforge-isaac-topic-control-1 \
  python /workspace/manyforge_core/tools/realtime_resource_profile_probe.py \
  --target controller-manager --command-line-token ros2_control_node
```

A passing Composer result proves exactly one `mf_sched_ctrl`,
`mf_sched_safe`, and `mf_sched_stop` worker with the requested policy,
priority, and affinity. It also proves every other Composer/DDS thread remains
`SCHED_OTHER` on the exact non-real-time set. A passing controller result
proves exactly one qualified update thread, no unowned controller RT thread,
all remaining controller/DDS threads on the non-real-time set, and nonzero
locked memory when requested.

The artifact also records the host kernel release, preemption marker, online
CPUs, effective cgroup CPUs, kernel command line, `isolcpus`, and `nohz_full`.
These host fields are informational rather than hidden behind the process-level
pass result.

## What a pass does not prove

Per-thread FIFO/affinity acceptance does not establish all of the following:

- a `PREEMPT_RT` kernel;
- exclusive host cpusets or IRQ affinity;
- full-tickless isolated CPUs;
- bounded firmware, driver, DDS, or hardware latency; or
- physical robot stop distance and controller deadline compliance.

Simulation and development profiles may therefore pass the process probe while
remaining non-qualified for physical deterministic timing. Release evidence
must pair the probe with loaded controller overrun/deadline metrics, ManyForge
runtime jitter and collision-to-stop evidence, viewer interference, target
power/thermal state, and the applicable robot hazard analysis.
