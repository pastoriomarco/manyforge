# TCPs

A TCP (tool centre point) is the point a move brings to its goal. This page
describes how Programs and Robot Control choose it. How a component's TCPs
are set up is in [Adding a component](ADDING_A_COMPONENT.md#set-up-a-components-tcps)
and on the component page ([workbench guide](../operations/DEVELOPMENT_WORKBENCH.md#tcps)).

## Terms

| Term | Meaning |
| --- | --- |
| **TCP entry** | One TCP, named by the component that owns it: component, target and frame, shown as `lite_gripper · gripper · tcp`. Every frame has exactly one name. |
| **Flange** | The robot's mount point for a tool. It is also one of the robot's TCPs. |
| **Component TCP** | A frame the component type declares or tags as a TCP, or a custom TCP. A frame merely named `tcp` is an ordinary frame. Each component has at most one default TCP. A descriptor's first `tcps` entry is its declared default. |
| **End-effector TCP** | Not a frame but a default mark on one TCP of a robot: the default TCP of the only tool on its flange, or the marked primary tool when there are several; otherwise the robot's own default TCP, or its flange. If the primary tool has no TCP, the robot has no End-effector TCP. |
| **Active TCP** | The TCP entry a robot currently uses. It starts at the End-effector TCP when one exists and may be none until a TCP is selected. |

## The robot's TCP list

A robot can use its own TCPs and the TCPs of every tool component in the
Deployment. The TCP pickers (a move's TCP, Set TCP, planning nodes and Robot
Control) show:

- the robot's own TCPs, including its flange;
- the TCPs of the tools mounted on it;
- the TCPs of tools mounted on no robot.

TCPs of tools mounted on another robot are hidden. The **Show TCPs of tools
mounted on other robots** checkbox (off by default) shows them too; its
tooltip reads "Useful only once tool change is implemented: today a TCP whose
tool is on another robot is refused when used." The pickers only change what
is shown, not what a Program may name.

TCP selectors are not checked against the robot's list when a Program is
applied. At run time a selected entry must resolve to a declared TCP and its
owner must be the robot or a tool it currently carries. An unmounted tool's
TCP can therefore be named in a Program but is refused before motion with
"Tool not mounted on this robot." (`tcp_tool_not_mounted`). A picker
checkbox changes visibility only; it does not change this run-time rule.

## The Active TCP

Each robot keeps its Active TCP while it is known:

| Event | Active TCP |
| --- | --- |
| Enable (also after Disable) | Kept. With **Send TCP offset to controller** selected, the controller is re-synced to the Active TCP, or offset 0 if there is none; with it off, Enable sends offset 0 (the flange) if needed. |
| Execute; Reset without **Reload initial scene**; Stop, Resume, Retry; Recover | Kept |
| A controller power cycle or vendor fault | Kept; the next Enable re-syncs the controller |
| A move whose TCP selector is a default TCP or a specific TCP; Set TCP; a selection in Robot Control | Set to that TCP |
| A move that replaces a running move | Never changed |
| Reset with **Reload initial scene**; **Reset simulation** (`simulation.reset`) | Reset to the End-effector TCP, or none if there is no End-effector TCP |
| An external Isaac world reload | Not observed by ManyForge; it does not reset the Active TCP. Use `simulation.reset` for a reset ManyForge observes. |
| A workbench restart, a robot process restart, New and Load, an Apply that restarts the robot, a Deployment change that removes the Active TCP | Starts again at the End-effector TCP, or none if there is no End-effector TCP |

A robot without an End-effector TCP has no initial Active TCP. Robot Control
shows "No End-effector TCP; choose one"; joint jog remains available, and
Cartesian control needs a selected TCP. Load and Apply remain available with
a non-blocking Deployment warning. Execute is refused while any robot in the
cell has no End-effector TCP (`program_end_effector_tcp_unavailable`), even if
a Program names another entry. Mark a TCP on the primary tool to restore the
End-effector TCP; see [Adding a component](ADDING_A_COMPONENT.md#set-up-a-components-tcps)
and the [workbench guide](../operations/DEVELOPMENT_WORKBENCH.md#tcps).

When a reset happens and **Send TCP offset to controller** is selected, the
controller is told the End-effector TCP's offset at once, or offset 0 if
there is no End-effector TCP. If it cannot take the offset now (disabled, not
prepared, in error, not connected yet), the reset still completes and the
next Enable re-syncs the controller. With the checkbox off, the reset changes
ManyForge's selection only; Enable sends offset 0 if the controller reports
another offset.

The Active TCP of each robot is also the read-only system key
`active_tcp.<target_role>` (see [Blackboard keys](BLACKBOARD_KEYS.md#7-system-keys)).

## TCPs in Programs

Each move names the robot it moves (**Moves**) and its TCP (**TCP**), in one
of three forms:

| Form | Meaning |
| --- | --- |
| **Current** | The robot's Active TCP. New moves start with it. |
| **Default TCP of <role>** | The default TCP of the component assigned to that role, for example the gripper's. It follows the Deployment: another gripper brings its own default. If the component has no default, using it is refused with a typed reason. |
| A specific TCP | One TCP entry, for example `gripper · cup_edge` or the robot's flange. |

A move with the second or third form also makes that TCP the robot's Active
TCP, so later **Current** moves use it. **Use current robot pose** captures
the pose of the move's TCP. Planning nodes use their own selector and do not
change the Active TCP.

**Set TCP** (Motion) sets a robot's Active TCP without moving, from a default
TCP of a role, a specific TCP, or **Current**. A specific entry may be supplied
directly from a `tcp_entry` key. **Current** uses the current Active TCP; it
does not select a different one. The node completes when the selected Active
TCP is reflected in the Program's key.

- It may run while the robot is not enabled. With offset sync selected, the
  controller gets the new offset at the next Enable. With sync off, Enable
  sends offset 0 (the flange) if the controller reports another offset.
- With **Send TCP offset to controller** selected, it is refused while a move
  of that robot is accepted or running ("robot busy"). With it off, it is
  allowed during a move; that is advanced use, and its consequences are
  yours (for example, a **Current** move that replaces the running move then
  names another TCP and is refused).
- Its **Stop behavior** works as for an I/O node. **Hold** (the default): a
  Stop before the TCP is sent means it is not sent and the node holds; a Stop
  after it is sent waits for the robot's answer, then holds. **Continue**: the
  node keeps going. Changing the TCP is never cut off halfway.

**A move that replaces a running move** (a preempt) never changes the Active
TCP. **Current** resolves to the current Active TCP without a controller
offset report. If the replacement resolves to a TCP other than the running
move's, it is refused on every lane
(`tool_context_change_during_motion`). With the checkbox off, changing the
Active TCP during a move can make a later Current preempt name a different
TCP, which is then refused by that same rule.

**Planning nodes** (Plan pick sequence, Plan place sequence) have their own
TCP selector, independent of the robot's Active TCP, and write their result
to their own output key. Keeping the planner's TCP and the moves' TCP
consistent is the Program's job: ManyForge plans and checks every move
(collisions, limits) before sending it, but does not compare it with the
planning node. The pick-and-place template gives the planner and its moves the
same selector (normally **Current**); you can change both.

**Legacy Program conversion.** When an older Program is opened, an old
`{robot, tcp}` becomes **Current** only when that moving robot's Program uses
no other TCP.
This is checked per moving robot; another robot's TCPs do not make it
ambiguous. If the same robot uses another TCP too, Load is refused with a
typed reason naming each affected move and asking you to choose its TCP. The
converter makes no guess. The same per-robot rule applies to old planning
nodes and pick-and-place selectors. An old `{tool, tcp}` becomes **Default TCP
of <that tool's role>**, resolved from the Deployment; a converted move to a
tool TCP uses the tool's contact bodies. The legacy-attach conversion uses
the Deployment's composed holder frames (type and custom frames) for a
non-robot holder. A real holder `tcp` stays named; if no composed `tcp` exists,
the removed duplicate
becomes **Active TCP**, and the holder role's `tcp` frame requirement is
removed. Without Deployment facts the Program remains unchanged and a missing
frame is refused at binding. A holder whose component has a trajectory
execution target is treated as a robot; its legacy literal `tcp` is not
converted, and an attach with `tcp` on the moving-arm role stays refused. For an offline CLI conversion of a 2.2 composite whose holder lacks a
composed `tcp`, binding refuses it; open it in Composer and choose the attach
frame. A concurrent external Apply during Program Open can change holder
frames after the conversion reads them; the Active TCP holder check still
applies when Attach runs.

## TCP speed limit

Every move is limited at its TCP, the robot's Active TCP during the move:
ManyForge sets that TCP on the robot, so it checks no other frame,
the flange included. The limit is the robot row's TCP ceiling
(`target_settings.arm.maximum_tcp_linear_speed_mps`); a move's own
`max_tcp_linear_speed_mps` can only lower it. Keep the ceiling at or below the
vendor TCP speed limit (`limits.max_tcp_linear_speed_mps` in the robot's
`component.yaml`); `scripts/manyforge component check --scenario` refuses a
ceiling above it ([Adding a component](ADDING_A_COMPONENT.md#descriptor-fields)).

- **Joint moves and time-optimal Cartesian moves** are timed by TOPP-RA with
  the TCP held at 0.98 of the limit at each point of its grid. A check of the
  trajectory's samples follows; when one is above the limit, the whole
  trajectory is slowed back to the 0.98 target. Speed between samples is not
  measured; the robot's own safety controller covers it.
- **Constant-speed Cartesian moves** run at 0.98 of the limit and are
  certified over the whole trajectory before they are sent.

The controller applies its own TCP speed limit at the TCP it was sent; with
**Send TCP offset to controller** off, that is the flange
([below](#the-controllers-tcp-offset)).

## TCPs and Apply

Choosing a component's default TCP (the Deployment row's `default_tcp`, on
the component page's **Default TCP and primary tool** card) is cell context: an Apply that changes only the default refreshes it without
restarting the robot. A custom TCP is part of
the component's model, so adding, changing or removing one replaces the
component at Apply (preview reason `description`), and a robot carrying that
tool too when the tool runs no process of its own. A replaced robot's Active
TCP starts again at its End-effector TCP.

## Robot Control

Robot Control shows and sets the robot's real Active TCP, the same value a
Program sees. Its **TCP** select shows the current status and lists the
robot's TCPs as above. When status is unavailable, Set TCP is unavailable.
The selector is disabled during a manual operation. Choosing a TCP sets the
Active TCP (it shows "Applying…" until confirmed or a newer status is
observed, or shows the refusal). Jog, Move to, the
Cartesian moves and the viewer gizmo use the Active TCP. When no Active TCP is
selected, joint jog remains available and Cartesian control needs a selected
TCP.

If a run is held and the Active TCP of one of its robots changes during
manual control, **Resume** asks "Resume with the new TCP?" and lists each
change as "<robot>: Active TCP is now <new> (was <old>)." The dialog offers
**Resume** and **Cancel**. This applies to Resume after a Full Stop too. The
question appears only when both values are known and differ; a page reload,
lost hold snapshot, unavailable value, replaced owner, or more than one held
group skips it. A new Run from Ready and API Resume do not ask. The hold and
TCP status streams are independent, so a delayed TCP status can cause a
dialog for a change that occurred before Hold.

## Attach to the Active TCP

An **Attach object** node names the holder (for example the gripper) and the
frame the part is attached to: one of the holder's frames, or **Active TCP**.
The Active TCP is resolved when Attach runs and is accepted only when it is
one of the holder's own TCPs; otherwise the attach is refused with "Active
TCP is not on the holder" (`attach_active_tcp_not_holder`). A later Active TCP
change does not retarget an existing attachment. Detach and grasp-lost
handling do not change.

On a Real xArm, the host follows its current Scene projection for total
payload and sends it through the xArm adapter's tool-context hook
(`ApplyToolContext`), the same call that sends TCP offsets. A successful projection transaction or rebuild prompts an update on
enabled robots, including while they move. When the total changes, it uses
`set_tcp_load` without sending a TCP offset or re-arming the controller. An
object without a declared payload, or with payload 0, does not change the total.
A composition failure or an unconfirmed update can fault the robot.

Enable and each new motion goal also read the host's current projection and
apply any needed payload update. An unseeded projection refuses the operation
with `execution_port_not_ready` without faulting the robot. Payload behavior
requires live qualification.

## The controller's TCP offset

A Real robot component has a **Send TCP offset to controller** checkbox in
the Deployment editor (selected by default). It decides whether the robot's
controller knows the Active TCP.

- **Selected**: a TCP change sends the offset to the controller while the
  robot is at rest, then settles/re-engages and checks that the reported
  offset matches. The move is sent only after this event step succeeds; an
  ordinary move with the same Active TCP sends no TCP offset and reads no
  offset report.
  If a sent offset is not confirmed, the component faults and the Active TCP
  remains at its last confirmed value. Recover clears the persistent
  component fault; the subsequent Enable re-syncs the controller to that
  Active TCP.
- **Not selected**: a TCP change only changes ManyForge's choice; no TCP
  offset is sent. The controller's configured offset target is 0 (the flange),
  and Enable sends zero if the controller reports another offset. Use it for
  instant TCP switching without sending offsets. The trade-off:
  everything the controller does or reports with its own TCP refers to the
  flange. Explicit Program and direct Scene attach/detach payload updates
  still run.

Payload synchronization is independent of this checkbox. The
details for UFACTORY arms are in
[Stack setup](../operations/STACK_SETUP.md#real-lite-6).

Without the Manager liveness stream, a very late response from a replaced
robot process can overwrite a newer Active TCP in the Program's copy. The
next result or ordered read corrects it; the robot still refuses a TCP on an
unmounted tool. If an xArm process crashes during an asynchronous offset send,
Enable can see a matching report and skip a send before the old request lands.
This very unlikely physical-offset residual is accepted and awaits live
verification; retiring an unknown request at Reset settles software state only.
