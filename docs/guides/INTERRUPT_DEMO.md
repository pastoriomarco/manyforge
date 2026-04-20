> Docs entrypoint: [`../INDEX.md`](../INDEX.md)

# Interrupt Demo

This guide documents the interrupt-focused pick-and-place demo shipped with
ManyForge.

Use it to validate the current industrial interrupt runtime baseline:

- external signal-triggered interrupt detection
- deterministic `hold_cycle` preemption
- named interrupt handler execution
- `resumable_hold` operator resume
- execution trace / blackboard / digest observability

## Files

- Launcher:
  [`../../scripts/demo-interrupt-pick-place.sh`](../../scripts/demo-interrupt-pick-place.sh)
- Program:
  [`../../examples/interrupt_pick_and_place.program.yaml`](../../examples/interrupt_pick_and_place.program.yaml)
- Deployment:
  [`../../examples/interrupt_pick_and_place.deployment.yaml`](../../examples/interrupt_pick_and_place.deployment.yaml)

## What The Demo Does

The main tree runs a simple pick-and-place sequence.

The deployment declares one manual interrupt source:

- `operator_pause`
  - adapter: `demo_ui`
  - signal: `pause_request`
  - kind: `edge`

The program binds that source to one interrupt policy:

- `operator_pause_hold`
  - preemption mode: `hold_cycle`
  - handler: `operator_pause_hold_handler`
  - after-handler disposition: `resumable_hold`

The handler itself is intentionally conservative:

1. set `demo/pause_latched = true`
2. invalidate cached trajectories

That means the demo exercises the real interrupt machinery without introducing
unsafe autonomous motion recovery.

## Start The Demo

From the repo root:

```bash
cd /home/tndlux/workspaces/dev_ws/src/manyforge
./scripts/demo-interrupt-pick-place.sh
```

Surfaces:

- Composer UI: `http://localhost:9000`
- Viser: `http://localhost:8081`

## Run The Cycle

```bash
curl -X POST http://localhost:9000/api/cycle/run
```

## Trigger The Interrupt

From another terminal:

```bash
curl -X POST http://localhost:9000/api/debug/signals/publish \
  -H "Content-Type: application/json" \
  -d '{"adapter":"demo_ui","signal":"pause_request","value":true,"staleAfterMs":1500,"mirrorKey":"demo/pause_request","metadata":{"reason":"operator_pause_demo"}}'
```

This publishes one external signal sample into the runtime signal registry and
mirrors it to `demo/pause_request`.

## Expected Behavior

When the interrupt fires during an active motion:

1. the interrupt is matched against deployment/program policy
2. the cycle enters the shared quiesce path used for controlled stop/hold
3. the kernel pauses the active execution
4. the interrupt handler runs
5. the cycle transitions to `STOPPED` with a pending interrupt in
   `resumable_hold`

Important:

- this is not the same semantics as pressing the UI stop button
- the interrupt path preserves cause, policy, handler, and disposition
- paused time no longer consumes execution timeout budget

## Resume

After the handler completes, resume with:

```bash
curl -X POST http://localhost:9000/api/cycle/run
```

Practical note:

- if the published signal is still fresh, resuming too quickly can retrigger the
  interrupt immediately
- for this demo, waiting about 2 seconds before resume is the simplest path

## Observe Runtime State

Execution digest:

```bash
curl http://localhost:9000/api/execution/digest | python3 -m json.tool
```

Look for:

- `state`
- `pendingInterrupt`
- `currentNode`
- `warning`

Blackboard snapshot:

```bash
curl http://localhost:9000/api/execution/blackboard | python3 -m json.tool
```

Look for:

- `demo/pause_latched`
- `_interrupt/current`
- `stop_requested`

Full debug export:

```bash
curl http://localhost:9000/api/debug/export | python3 -m json.tool
```

Useful sections:

- `execution.history`
- `execution.signalSnapshot`
- `execution.kernelSnapshot`

`ros2_medkit`-style export:

```bash
curl http://localhost:9000/api/debug/export/ros2-medkit | python3 -m json.tool
```

## Reset

If you want to restart from a clean state:

```bash
curl -X POST http://localhost:9000/api/cycle/reset
```

Then run again:

```bash
curl -X POST http://localhost:9000/api/cycle/run
```

## What This Demo Proves

This demo is useful because it validates the current interrupt-runtime contract
end to end:

- external signals can drive interrupt sources
- interrupt policy matching is deterministic
- stop/interrupt quiesce behavior is unified at the low-level runtime path
- handlers execute under bounded semantics
- operators can inspect the full interrupt lifecycle through the observability
  endpoints

It does not yet prove:

- motion-bearing interrupt handler auto-execution
- nested interrupt handling
- ROS/PLC adapter integration
- ROXSIE-specific signal mapping
