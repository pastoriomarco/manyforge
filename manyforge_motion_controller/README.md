# manyforge_motion_controller

This ros2_control plugin consumes the canonical shared `StepMotion` numerical
implementation through the small sibling `manyforge_core/motion_numerics`
CMake target. It loads and configures while inactive. Production activation
remains unavailable: execution-host wiring, an independent stall watchdog,
qualified hardware delivery and the selected timing profile are still absent.
There is no ROS command ingress, fake-mode parameter or advertised motion
capability.

The `joints` parameter requests one to sixteen ordered, unique position command
interfaces and position/velocity state interfaces. Configuration never writes
commands. The plugin's update method invokes the receiver's actual Tick when
host wiring supplies a receiver; without one it returns `ERROR`. Offline tests
supply a receiver through a private C++ test peer and loaned interfaces, with an
injected receiver-local clock. ROS time and supplied update durations never
replace that clock.

`ControllerMechanism` implements the frozen `IExecutionMechanism` port. Its
NRT profile requires the exact binding, resource order, initial accepted scale
and generation, command-realization contract and local-clock function. Motion
bounds, interpolation, tolerances and tick envelope come from immutable Core
prepared input; the adapter supplies no replacement motion or Stop policy.
Replace checks P/V/A continuity against the old goal's actual current Tick.
A rejected installation reports consumption with known no command effect and
releases the rejected borrow. Pending Stop takes precedence over replacement.

Four fixed records bound **outstanding** prepared borrows, not lifetime moves.
A four-event ordinary queue consumes at most one event per Tick. Exhaustion
refuses before effect with `ChannelFull`. Stop and scale use separate latest
fixed-payload channels per record; Hold/Cancel/Fence use a versioned atomic
fence request. Short per-record NRT publication mutexes serialize concurrent
callers. No transport, ordinary queue work or allocation holds those mutexes;
Stop never acquires the ordinary or lease mutex and never returns ChannelFull.
The update thread does not allocate, own shared metadata, wait for a mutex or
retry a channel exchange. All loaned interface names, order and double data types are checked before
any write. Loaned interface writes use one bounded attempt.
ROS handle locking and configured command limiters still need qualification in
the eventual deployment profile; these offline fixtures use valid double
handles with the default limiter.

Full keys stay in immutable NRT shared metadata. RT events and snapshots carry
record index/version and fixed goal fences. Release clears only its matching
borrow version, after the loop drops its prepared pointer. A record can be
reused after a status read observes that release fact; all delayed old events
and channel messages remain version-fenced. The caller must retain prepared
storage until `slot_referenced == false`. Lifecycle teardown requires update
quiescence. Reclaimed records are not a history journal; the execution host
retains observed per-key facts through the existing shared execution owner.

A successful local write counts as realized only for the explicitly supplied
`LoopInterfaceWrite` contract. An Isaac `TransportPublication` contract records
computed commands without claiming publication or delivery. Failed partial
writes retain partial effect and fence future writes. A later failed frame
cannot erase a previously observed goal effect. Accepted scale/Stop facts are
attributed only to their governing written frames; ignored or invalid targets
cannot inherit dispatch realization. Scale reaches the realized phase only
when the accepted target and zero scale rate occur in a written frame. Local Hold/Cancel/Fence,
release and containment report only `FenceRequested`: downstream queues may
still act, and no `Fenced`, `Drained`, delivery, stationary or completion fact is
fabricated. Numerical path end alone is never completion.

Lease renewal uses local NRT receipt time plus the caller's duration. Tick
checks expiry even at scale zero. Expiry latches local publication containment;
renewal cannot resume. Stop deadlines use actual local time even when the
shared numerical step caps late advancement. The independent watchdog's
checked timestamp remains zero, explicitly absent. Timing and hardware safety
qualification remain outstanding.

Tests exercise shared-step parity, installed plugin lifecycle, the actual
plugin update with loaned handles, reserved Stop under full ordinary traffic,
invalid/deadline Stop containment, lease expiry/renewal, partial writes, stale
keys, concurrent publishers, record reuse beyond capacity and delayed stale
messages. Tick allocation checks cover successful Stop and interface-count
failure. Run the package GTests and ament lint in the pinned Jazzy diagnostic
image with canonical source inputs; no simulator or hardware is exercised.
